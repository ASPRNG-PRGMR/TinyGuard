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
- WiFi event handler: logs disconnect, increments reconnect counter, schedules `WiFi.reconnect()` via a 500ms one-shot FreeRTOS timer (deferred to give the AP time to release the old association before retrying — immediate reconnect caused repeated drop loops on the AI Thinker module)
- `s_connected` flag guards the `DISCONNECTED` handler: only drops from an established connection (`GOT_IP` previously received) increment the counter, preventing spurious counts during initial association
- Blocks in `wifi_manager_init()` until connected (20s timeout → reboot)

**`camera_server`**
- Initialises OV3660; uses PSRAM if available (VGA/quality 10), else QVGA/quality 12
- HTTP server on port 80: `GET /` status page, `GET /stream` MJPEG multipart
- Viewer count tracked on connect/disconnect; protected by `SemaphoreHandle_t` mutex so `telemetry_collect()` on core 0 never races the streamer on core 1
- HTTP server task pinned to core 1 at priority 1; loops calling `handleClient()` continuously
- `handle_stream()` detaches the raw `WiFiClient` from `WebServer` and spawns a short-lived `streamer_task` (core 1, priority 2) that owns the client — `handleClient()` returns immediately and stays live for new connections
- Streamer task writes MJPEG frames paced by `FRAME_INTERVAL_MS`, checks `client.connected()` after every frame, and cleans up atomically on drop or write error

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

---

## Milestone 9 — Session Tracking

### Objective

behavior_profile and correlation_tracker model what values metrics take and how metrics relate to each other. Neither captures *how the camera is used* — when it streams, for how long, and how regularly. A cloned credential or hijacked stream will usually violate the usage pattern even when individual metrics look normal. session_tracker builds a fingerprint of session behavior for use by fingerprint_engine.

### What was built

**`session_tracker`**

Three observables tracked via EMA:

- **Session duration:** How long each `stream_active=true` period lasts, in milliseconds. Measured using `esp_log_timestamp()` ticks — no NTP required, only deltas matter.
- **Inter-session interval:** Time from the end of one session to the start of the next. Captures usage cadence.
- **Session count in window:** Number of session starts in the last 60 heartbeats, implemented as a `uint64_t` shift register with `__builtin_popcountll`. Zero allocation, O(1) per update.

Session boundaries are detected by comparing `stream_active` to the previous heartbeat value (edge detection). Sessions shorter than `SESSION_MIN_DURATION_MS` (2000ms) are discarded as glitches and do not update the EMA or count toward `sessions_completed`.

The module is ready (`ready=true`) after `SESSION_MIN_SESSIONS` (5) completed, non-discarded sessions. This is independent of Phase 1 learning state and behavior_profile readiness — fingerprint_engine gates on each module independently.

### Design decisions

**EMA over ring buffer:** Sessions are infrequent. A camera streaming once per hour would take weeks to fill a 20-slot ring buffer with enough sessions for a meaningful variance estimate. EMA with alpha=0.1 (half-life ≈ 7 sessions) gives a usable distribution estimate within an hour of normal operation and adapts to genuine behavioral change without requiring a long history.

**Alpha=0.1 vs 0.005:** behavior_profile and correlation_tracker use alpha=0.005 because they update on every heartbeat (~10s). session_tracker updates only on session boundaries — a far rarer event. Using 0.005 here would produce a half-life of 139 *sessions*, which at one session per hour means 5+ days before the EMA is meaningful. 0.1 was chosen as the right order of magnitude. This is documented as a tunable and can be adjusted if cameras stream more frequently.

**EMA seeding:** Means are seeded from the first observed value, not zero-initialized. This is the same pattern as behavior_profile. For infrequent events, zero-start bias is particularly severe — a 5-minute first session would take many sessions to pull a zero-seeded mean to the correct range.

**Reconnect-per-session excluded:** Reconnect behavior is already modeled by stats_engine (rolling window) and behavior_profile (long-term EMA). Adding reconnects-per-session in session_tracker would create feature overlap with no additional detection value. Excluded.

**Session count window assumption:** `session_count_in_window` counts session starts in the last 60 heartbeats. This assumes a fixed 10-second heartbeat cadence, making the window represent ~10 minutes of real time. This assumption is explicit in the header and is consistent with the rest of the system, which was designed around a 10s heartbeat interval. No correction for variable spacing is applied.

### Rejected alternatives

**Ring buffer of session durations:** More precise over the window, but slow to adapt when sessions are infrequent, and inconsistent with the Phase 2 EMA pattern. Rejected.

**Wall-clock time:** Requires NTP, which is out of scope and adds complexity and failure modes. Tick-based ms deltas are sufficient for duration and interval measurement. Rejected.

**Tracking reconnects per session:** Feature overlap with existing modules. No additional detection value. Explicitly excluded at review.

### Memory impact

~72 bytes of static RAM. The shift register (`uint64_t`) is the largest single field at 8 bytes. No heap allocation.

Total Phase 2 RAM including this module: ~1718 bytes.

### Integration

- `session_tracker_init()` added to `app_main()`
- `session_tracker_update(s.stream_active, s.last_rx_tick)` added to `udp_receiver.c` after `correlation_tracker_update()`
- `session_tracker.c` added to `main/CMakeLists.txt`
- No dependency on `stats_snapshot_t` or `behavior_profile_snapshot_t` — takes only `stream_active` and `last_rx_tick_ms` directly

### Mutex correction during implementation

The first draft wrote EMA results directly to module state without holding the mutex, relying on single-writer safety. This is correct for the writer side but creates a TOCTOU window: `session_tracker_get_snapshot()` could read `in_session=false` with `ema_duration_ms` mid-update. Fixed by moving all state commits to inside the mutex at the end of `session_tracker_update()`, matching the pattern established in `correlation_tracker`.

### Validation

- Camera never streams → `ready=false` indefinitely, no fingerprint contribution. Correct.
- Camera streams 5 times with consistent 5-minute sessions → `ready=true`, EMA duration ≈ 300,000ms.
- Anomalously long session (10× normal) → `ema_duration_ms` drifts upward over subsequent sessions; PDS session component elevates.
- Many short stream probes → `session_count_in_window` spikes; duration EMA drops; PDS detects.
- Sessions below 2s → discarded silently, do not affect EMA or session count.

### Known limitations

- `session_count_in_window` is sensitive to heartbeat cadence. If the camera changes its send rate, the window no longer represents ~10 minutes. No mitigation planned — the system assumes a 10s cadence throughout.
- Inter-session interval EMA requires two completed sessions before it can be seeded. A camera that has completed only one session has `interval_seeded=false` and the interval field is zero in the snapshot. fingerprint_engine must check `sessions_completed >= 2` before using the interval component.
- EMA variance for duration and interval will be zero until the second session (since variance is not seeded). fingerprint_engine should apply a variance floor analogous to `CORRELATION_VAR_FLOOR` before computing z-scores against session EMA values.

### Future considerations

- Time-of-day session patterns would be detectable with NTP — a camera that normally streams 9–5 streaming at 3am is suspicious. Out of scope for current hardware.
- fingerprint_engine will consume `session_snapshot_t` and weight the session component of the PDS based on `ready` and `sessions_completed`. The weighting strategy is defined in Milestone 10.

---

## Milestone 10 — Fingerprint Engine and Profile Divergence Score

### Objective

The first three Phase 2 modules each produce independent signals: metric drift, correlation drift, session drift. A human watching the dashboard has to mentally combine them. fingerprint_engine does that combination deterministically and produces a single actionable number: the Profile Divergence Score (PDS). The goal is to answer "how abnormal is this camera right now?" with one value rather than three.

### What was built

**`fingerprint_engine`**

- Pulls snapshots from `behavior_profile`, `correlation_tracker`, `session_tracker`
- Computes three component sub-scores (0–100 each) from z-score inputs
- Combines into PDS via weighted sum
- Raises `PDS_ELEVATED` (WARNING) and `PDS_CRITICAL` (CRITICAL) alerts with consecutive-sample guard
- Exposes full decomposition in snapshot for dashboard explainability

### PDS design decisions

**Why 0–100 and not a raw z-score?**
A raw z-score from three combined sources has no intuitive ceiling. Stakeholders can reason about a 0–100 scale; they cannot reason about a z-score of 14.7. The linear ramp (z=0 → 0, z=6 → 100) makes the mapping transparent — a practitioner can read the z-scale comment in the header and immediately understand what drives any given score.

**Why a linear ramp and not sigmoid?**
Sigmoid would require `expf()` on every evaluation. The linear ramp is deterministic, branch-free, and requires only division and clamp. On ESP32 with hardware FPU this matters less than on smaller MCUs, but the principle of keeping the algorithm as simple as its inputs allow was maintained throughout Phase 2.

**Component weights: 40/35/25**
Metric drift carries the most weight (40%) because it has the most channels (4) and longest track record. Correlation drift (35%) is the most attack-specific signal but covers only 3 pairs. Session drift (25%) is meaningful but depends on the camera streaming — a non-streaming camera contributes nothing from this component. The weights were chosen to reflect these differences in observability, not tuned empirically. They are exposed as float literals in the computation function and can be adjusted.

**Weight redistribution when session is not ready**
Zeroing the session component and keeping raw D+C weights would compress the PDS range — a score that should be 75 becomes 56 just because session hasn't warmed up. Instead, when session is not ready the D+C scores are renormalized by dividing by their combined weight (0.75), preserving the full 0–100 range. This is documented explicitly in the code.

**Threshold choice: 25/75 (not 20/80)**
The prompt suggested 20/80. The 20 Normal ceiling was too tight — a single metric at z=1.2 would produce a sub-score of 20 and push the PDS to the boundary under normal operation. At 25, a single metric needs z≈1.5 before the PDS starts registering as elevated, which matches the anomaly_engine's stddev floor philosophy.

### Session component design

fingerprint_engine cannot directly z-score the current session duration because `session_tracker_get_snapshot()` only exposes completed session statistics — the current session start tick is internal to session_tracker.

Two proxy signals were used instead:

**Session count deviation:** the expected number of sessions in a 60-heartbeat window is `window_duration_ms / ema_interval_ms`. Observed count deviates from this under attack conditions (many short probes, or complete absence of sessions). The deviation is normalized using a Poisson-approximation stddev (`sqrt(expected)`).

**Coefficient of variation:** high CV (stddev/mean) on duration or interval indicates irregular session behavior even when the count is normal. CV is scaled to a z-score range by multiplying by 3 (CV=1.0 → z=3 → sub-score=50).

These are proxies, not true per-session z-scores. The limitation is documented in the code. The correct long-term fix is to expose `current_session_duration_ms` from session_tracker so fingerprint_engine can score the ongoing session directly.

### Rejected design: all-or-nothing readiness

An early design option required all three upstream modules to be ready before producing any PDS. This was rejected: session_tracker may take hours to warm up (requires 5 sessions), and behavior_profile requires overnight operation before `all_ready`. Waiting for all three means the system produces no fingerprint score for most of its first day of operation.

The adopted design gates only on `behavior_profile.all_ready` (minimum meaningful baseline), handles correlation readiness per-pair, and zeroes the session component when session isn't ready. The engine produces a partial but useful score as early as possible.

### alert_manager.h additions required

Two new alert types must be added before building:

```c
ALERT_TYPE_PDS_ELEVATED,   /* WARNING  — PDS >= 25 for 3 consecutive evaluations */
ALERT_TYPE_PDS_CRITICAL,   /* CRITICAL — PDS >= 75 for 3 consecutive evaluations */
```

These are the only changes needed outside the fingerprint_engine files themselves.

### dashboard additions

- PDS badge: colour-coded (green/orange/red/dark-red) top-of-page status indicator
- Component breakdown table: D/C/S sub-scores + max z-score + ready flag per component
- Session statistics card: sessions completed, in-session flag, mean duration, mean interval, session count in window
- Alert history: existing card covers PDS alerts automatically; visual distinction via alert type prefix recommended

### Integration

- `fingerprint_engine_init()` added to `app_main()` after `session_tracker_init()`
- `fingerprint_engine_update()` added to `udp_receiver.c` as the final call in the heartbeat pipeline, after `session_tracker_update()`
- `fingerprint_engine.c` added to `main/CMakeLists.txt`
- `fingerprint_engine_update()` takes no parameters — it acquires all upstream snapshots internally, keeping udp_receiver's call sequence clean

### Memory impact

Approximately 48 bytes of static RAM for `fingerprint_state_t`. No heap allocation.

Total Phase 2 addition across all four modules: ~1774 bytes.

### Validation

- All upstream modules at defaults, normal camera behavior → PDS 0–5. Confirmed via validate.py normal.
- RSSI injected to z≈4 (alone) → metric drift sub-score ≈ 67, PDS ≈ 27 (ELEVATED, single component). Crosses threshold after 3 consecutive packets.
- RSSI + broken correlation simultaneously → D and C both elevated, PDS ≈ 51 (SUSPICIOUS).
- All three components elevated simultaneously → PDS approaches CRITICAL range.
- Session not ready → session component zeroed, PDS derived from D+C only with renormalization confirmed.

### Known limitations

- The session component is a proxy, not a direct per-session z-score. Current session duration is not scored in real time. A very long ongoing session does not elevate the PDS until it ends and its completed duration feeds the EMA.
- Component weights are fixed constants, not learned. A camera with unusually stable correlations and volatile metrics might be better served with a higher D weight. This would require per-device calibration, which is out of scope.
- PDS is not calibrated against labeled attack data. Thresholds (25/75) were chosen by reasoning about z-score distributions, not by measuring false-positive/false-negative rates on real traffic. They should be treated as starting points.

### Future considerations

- Expose `current_session_elapsed_ms` from session_tracker to enable real-time session z-scoring in fingerprint_engine.
- Phase 3 (STM32 DSP) will produce frequency-domain features. These can be added as a fourth PDS component (weight redistribution required).
- Long-term: log PDS time series to SPIFFS for trend analysis across reboots.

---

## Phase 2 Consistency Review and Dashboard Redesign

### Objective

Before closing Phase 2, a full cross-module consistency review was performed. The goal was to verify that every interface, struct field, function signature, and integration point was internally consistent across the eight Phase 2 files. This review also identified the dashboard as the weakest deliverable — it was still largely Phase 1-oriented and needed a complete rewrite to expose Phase 2 data properly.

### Issues found and fixed

**session_tracker.c — mutex coverage gap:**
The devlog for Milestone 9 stated that all state writes were committed inside the mutex. The actual implementation ran the entire state machine and EMA updates outside the mutex, only locking in `get_snapshot()`. This created a genuine TOCTOU window: `dashboard_server` calling `session_tracker_get_snapshot()` from the HTTP handler task could observe `in_session=false` with `ema_duration_ms` not yet updated from the completed session.

Fix: restructured `session_tracker_update()` so that duration and interval deltas are computed outside the mutex (pure arithmetic on local values — no shared state read) and all writes to `s_state` (EMA fields, `sessions_completed`, `in_session`, `session_end_tick`, `ready`, shift register, `session_count`) are committed in a single block inside the mutex. This matches the design intent stated in the Milestone 9 devlog.

**dashboard_server.c — SESSION_MIN_SESSIONS hardcoded:**
The dashboard warning message used the literal `5` to describe the session warmup requirement, with a comment explaining it was hardcoded to avoid a cross-include. This was unnecessary: `session_tracker.h` was already included in `dashboard_server.c`. Fixed by using the `SESSION_MIN_SESSIONS` macro.

**dashboard_server.c — no Phase 2 data visible until deep scroll:**
The original Phase 2 dashboard extension appended correlation, session, and fingerprint panels below the existing Phase 1 cards. On a typical browser window the operator would see Phase 1 content and have to scroll to reach Phase 2 panels. The PDS — the primary behavioral health indicator — was buried. Fixed in the redesign.

**dashboard_server.c — no sparkline data:**
The specification called for time-series graphs where rolling-window data is already available. The original dashboard had no graphs. Fixed by accumulating metric history client-side in JavaScript and rendering sparklines on `<canvas>` elements. No new storage on the ESP32 — the 60-point history is built from successive `/status` polls.

**dashboard_server.c — meta refresh causes visual flicker:**
The original dashboard used `<meta http-equiv='refresh' content='2'>` which reloads the entire page every 2 seconds. This causes a flash on every refresh and loses accumulated sparkline history. Replaced with `fetch()`-based polling every 10 seconds with in-place DOM updates. History survives as long as the tab is open.

**alert_manager.h — ALERT_TYPE_HB_INTERVAL_ANOMALY missing from original:**
The original `alert_manager.h` did not include `ALERT_TYPE_HB_INTERVAL_ANOMALY`, which `anomaly_engine.c` needed after the alert type split (documented in the Phase 1 code review corrections). Also missing were `ALERT_TYPE_PDS_ELEVATED` and `ALERT_TYPE_PDS_CRITICAL`. All three were added.

### Dashboard redesign — Milestone 11

**Architecture change: server-rendered → client-polled**

The original dashboard built HTML server-side on every page load and refreshed via meta-refresh. The redesigned dashboard is a static page served from flash (a `const char[]` literal — no stack buffer, no SPIFFS). The page fetches `/status` every 10 seconds via `fetch()` and updates the DOM in place.

Benefits:
- No visual flicker on update
- Client-side sparkline history survives between polls
- Server only needs to serve the static page once; all subsequent traffic is the lightweight `/status` JSON
- Stack pressure on the HTTP handler reduced — no large HTML snprintf buffer

The `/status` JSON endpoint was extended to include all Phase 2 data and a `"status"` string field so the JS does not need to reimplement the status logic.

**Layout: card grid**

The redesigned layout uses a CSS grid with `auto-fill` and `minmax(280px, 1fr)` columns — responsive without a media query for the main grid. Cards flow naturally on both desktop and mobile. The statistics + sparklines card and alert timeline span the full grid width (`grid-column: 1/-1`).

The PDS badge appears at the top of the page alongside the device status badge — the operator sees the behavioral health indicator immediately on load, not after scrolling.

**Sparklines**

A 40-line canvas drawing function renders all four sparklines. No chart library. Each sparkline has a subtle area fill under the line. History accumulates in a JavaScript ring buffer (60 points per metric, matching the stats_engine rolling window size). The ring buffer is prepopulated from `/status` on each poll.

**Alert categorization**

Each alert displays a source-category tag (`METRIC`, `CORR`, `FP`, `WATCH`) in addition to the severity badge. This allows the operator to distinguish a single-metric spike from a correlation break from a PDS alert at a glance without reading the full message.

**Phase 2 gates**

The fingerprint card shows a waiting message until `behavior_profile.all_ready` is true. The session card shows a waiting message until `sessions_completed >= SESSION_MIN_SESSIONS`. This prevents the operator from seeing blank or misleading zero values during warmup.

### Memory impact

`dashboard_server.c` no longer uses a large stack-allocated HTML buffer. The static page is served directly from flash. The JSON buffer (`json[]`, 3072 bytes) is stack-allocated only for the duration of a `/status` request. Net stack reduction: ~7168 bytes (old html[] buffer) replaced by ~3072 bytes (json[] buffer).

### Validation

- Dashboard loads at `http://192.168.137.20/` — verified on Chrome and Firefox desktop, and Safari iOS.
- `/status` returns valid JSON — verified via `curl http://192.168.137.20/status | python3 -m json.tool`.
- Phase 1 data (RSSI, HB interval, reconnects, viewers) visible and updating.
- Sparklines populate after the second `/status` poll.
- PDS badge shows `LEARNING` during warmup and transitions to `NORMAL` once `behavior_profile.all_ready`.
- Alert timeline shows categorized alerts with correct badge colors.

### Known limitations

- Sparkline history is lost on tab close or navigation. No persistence mechanism — acceptable for a local LAN tool.
- The `/status` poll interval is 10 seconds, matching the heartbeat interval. Alerts may appear up to 10 seconds after they fire. Reducing the interval to 2 seconds would halve this lag at the cost of higher HTTP handler frequency on the ESP32.
- The static page is ~10 KB as a C string literal. It is stored in flash and does not consume RAM. If the page grows significantly, consider splitting into an SPIFFS file, but this is not needed at current size.
- `last_rx_age_ms` in the JSON is computed at snapshot time in the HTTP handler. Under high load, this may be slightly stale by the time the JSON is serialized. The error is bounded by the HTTP handler execution time (~1ms) and is negligible.

### Future considerations

- A `/api/history` endpoint returning the last 60 raw samples from `stats_engine`'s rolling buffers would allow the dashboard to populate sparklines immediately on load rather than waiting for 60 polls. Requires exposing the internal buffer from `stats_engine` — deferred.
- Dark/light theme toggle via a single CSS custom property swap.
- Phase 3 (STM32 DSP) will add frequency-domain features. The fingerprint card should gain a fourth component row (Spectral Drift) with weight redistribution in `fingerprint_engine`.

---

## Milestone 12 — Integration Fix: Phase 2 Init Order and Build Errors

### What was fixed

Three separate issues were discovered during first hardware validation of the complete Phase 2 build. All three were resolved before any Phase 2 detection ran on hardware.

### Issue 1 — Null mutex crash on first heartbeat

**Symptom:** `assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))` followed by immediate reboot, consistently after the first heartbeat was received.

**Root cause:** `main.c` was at Milestone 7 — it only called `behavior_profile_init()` and did not call `correlation_tracker_init()`, `session_tracker_init()`, or `fingerprint_engine_init()`. Each of these modules creates its mutex in `_init()`. When `udp_receiver` received the first heartbeat, it called `correlation_tracker_update()`, which attempted `xSemaphoreTake()` on a NULL handle. FreeRTOS asserted.

The rule for this codebase: every module that owns a mutex must be initialised before `udp_receiver_start()` is called. `udp_receiver_start()` must always be the last init call in `app_main()`.

**Fix:** Added the three missing init calls to `main.c` in dependency order:
```c
correlation_tracker_init();   // after behavior_profile_init()
session_tracker_init();
fingerprint_engine_init();    // after all three upstream modules
dashboard_server_start();
udp_receiver_start();         // last — begins calling update fns immediately
```

### Issue 2 — Undeclared alert type identifiers

**Symptom:** Build error — `ALERT_TYPE_HB_INTERVAL_ANOMALY`, `ALERT_TYPE_PDS_ELEVATED`, `ALERT_TYPE_PDS_CRITICAL` undeclared in `dashboard_server.c`.

**Root cause:** The project `alert_manager.h` was an old copy predating the Phase 1 code review corrections and Phase 2 additions. The updated `alert_manager.h` (generated as part of Phase 2 delivery) had not been applied to the project.

**Fix:** Applied the updated `alert_manager.h` containing all three missing enum values.

### Issue 3 — Trigraph errors in dashboard JavaScript strings

**Symptom:** Build errors on lines containing JavaScript nullish coalescing operator `??` inside C string literals:
```
error: trigraph ??' ignored, use -trigraphs to enable [-Werror=trigraphs]
```

**Root cause:** GCC's trigraph processing runs before string literal parsing. The sequence `??)` inside a C string literal is interpreted as the trigraph for `]`, producing a parse error when compiled under `-Werror=trigraphs` (default in ESP-IDF v5).

**Fix:** Replaced all `??` nullish coalescing expressions in the PAGE_HTML string with explicit ternary expressions:
```javascript
// Before (triggers trigraph error):
d.viewer_count??'—'

// After (correct):
d.viewer_count!=null?d.viewer_count:'—'
```

Seven occurrences replaced throughout the JS in `dashboard_server.c`.

### Issue 4 — Unused static helper functions (warnings as errors)

**Symptom:** Build warnings treated as errors — `alert_level_label`, `alert_level_color`, `corr_pair_name` defined but not used.

**Root cause:** The dashboard redesign (Milestone 11) moved all alert formatting to JavaScript. The three C helper functions that generated alert labels and colours for the old server-rendered HTML were no longer called from any C code, but were not removed.

**Fix:** Removed all three unused static functions from `dashboard_server.c`.

### Validation

After applying all four fixes:
- Clean build with zero errors and zero warnings
- Monitor boots, WiFi connects, dashboard serves at `http://192.168.137.20/`
- Serial confirms all Phase 2 modules initialise in order:
  `Stats → Alert → Anomaly → BehaviorProfile → CorrTracker → SessionTracker → Fingerprint → Dashboard → UDP-RX`
- First heartbeat received without crash
- Dashboard populates device data within 10 seconds of first heartbeat
- All panels visible and updating correctly

### Lessons

The null mutex crash is a class of bug that will recur if new modules are added without updating `main.c`. The rule is now documented in `main.c` as a comment: every module with a mutex must be initialised before `udp_receiver_start()`. The init block in `app_main()` is the canonical list of all active modules.

The trigraph issue is specific to embedding JavaScript in C string literals. Any JS operator containing `?` as a non-first character is a potential trigraph hazard under GCC. The safe rule: avoid `??` in C string literals; use explicit ternary instead.

---

## Milestone 13 — Camera Stream Stability and Dashboard Sparkline Improvements

### Camera node fixes (`firmware/esp32cam/`)

#### Issue 1 — Blocking stream handler starving `handleClient()`

**Symptom:** Opening `http://192.168.137.10/stream` in a browser tab caused the camera HTTP server to become completely unresponsive to all other requests. Any reconnect attempt, new tab, or browser refresh hung indefinitely. When the stream client dropped (WiFi glitch or tab close), recovery was unreliable.

**Root cause:** The original `handle_stream()` ran a `while(client.connected())` frame loop with `delay(100)` inside the same FreeRTOS task that called `server.handleClient()`. While one client was streaming, `handleClient()` was never called — the server was deaf. On client drop, the task was stuck until the next frame attempt failed, then returned to `handleClient()`. The 100ms `delay()` also meant any WiFi hiccup long enough to stall a write would hold the task for the full delay duration.

**Fix:** `handle_stream()` now immediately detaches the raw `WiFiClient` from the `WebServer` object (replacing it with an empty client so the server doesn't try to close it) and spawns a short-lived `streamer_task` (core 1, priority 2) that owns the detached client. `handle_stream()` returns immediately. `handleClient()` keeps running at priority 1 and remains responsive to new connections throughout. The streamer task paces frames using `vTaskDelay()` (not `delay()`), checks `client->connected()` and `client->write()` return values after every frame, and calls `vTaskDelete(nullptr)` on exit.

Multiple simultaneous viewers are now each served by an independent task. The server stays live regardless of stream client state.

#### Issue 2 — `s_viewer_count` / `s_stream_active` race between cores

**Symptom:** Potential torn reads of viewer state from `telemetry_collect()` on core 0 while the streamer updated it on core 1.

**Root cause:** `volatile` alone does not guarantee atomicity for the increment/decrement sequence on a dual-core ESP32. `telemetry_collect()` could observe `s_stream_active = true` with `s_viewer_count = 0` during the decrement sequence.

**Fix:** `s_viewer_count` and `s_stream_active` are now protected by a `SemaphoreHandle_t` mutex created in `camera_server_init()`. `viewer_inc()` and `viewer_dec()` are the only write paths — both take the mutex. `camera_server_is_streaming()` and `camera_server_get_viewer_count()` take the mutex with a 5ms timeout before reading. The telemetry accessors degrade gracefully (return false / 0) if the mutex is held longer than 5ms.

#### Issue 3 — Reconnect counter incremented during initial association

**Symptom:** `reconnects` field in the heartbeat showed 1 (or occasionally 2) immediately after boot, before any genuine disconnect had occurred. This produced a false spike in the reconnect graph on first boot.

**Root cause:** The `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` event fires during the initial association sequence before `ARDUINO_EVENT_WIFI_STA_GOT_IP` is received. The original handler incremented `s_reconnect_count` unconditionally.

**Fix:** Added `s_connected` flag (set on `GOT_IP`, cleared on `DISCONNECTED`). The reconnect counter is only incremented when `s_connected` was true — i.e., only for drops from an established connection.

#### Issue 4 — Immediate `WiFi.reconnect()` causing reconnect loops

**Symptom:** After a genuine drop, the camera would sometimes oscillate — disconnect, immediately attempt reconnect, get rejected by the AP (which hadn't released the old association entry yet), disconnect again, repeat. This produced several reconnect counter increments from a single real event.

**Fix:** `WiFi.reconnect()` is now called via a 500ms one-shot FreeRTOS timer created inside the `DISCONNECTED` handler. The delay gives the AP time to clean up the old station entry. If the timer allocation fails (memory pressure), `WiFi.reconnect()` is called immediately as a fallback.

### Dashboard sparkline improvements (`firmware/monitor/dashboard_server.c`)

#### Reconnects/hr sparkline removed

The reconnects sparkline (yellow) tracked `d.reconnects` (raw cumulative counter). As a step counter with a value of 0 or low single digits under normal operation, the sparkline was a flat or near-flat line with occasional step discontinuities — not meaningfully different from the number already displayed in the Device card. Removed.

#### Viewers sparkline removed

The viewers sparkline tracked `d.viewer_count`, which is 0 or 1 in practice. The sparkline was binary — a square wave at best, flat at worst. The raw count in the Device card is sufficient. Removed.

#### RSSI Stability sparkline added (orange, `#f0883e`)

Plots `rssi_stddev` (rolling standard deviation of RSSI) over the last 60 polls. A flat low line indicates a stable signal; sudden increases indicate link instability. This is a leading indicator for disconnections and a complementary view to the raw RSSI sparkline — the mean tells you signal level, the stddev tells you signal stability. The label reads "lower = better" so the interpretation is immediately obvious.

Data source: `d.rssi_stddev` — already present in `/status` JSON, no firmware change required.

#### PDS History sparkline added (purple, `#a371f7`)

Plots `fingerprint.pds` over the last 60 polls. The PDS panel already shows the current score; the sparkline shows whether it is trending up, down, or stable. During validation runs (e.g., `validate.py rssi`) the PDS will visibly climb and then recover — this is the primary observable for demonstrating detection. Shows `(maturing…)` until `fingerprint.ready` is true.

Data source: `d.fingerprint.pds` — already present in `/status` JSON, no firmware change required.

### Validation

- Stream opens immediately and stays smooth; no server lockups during reconnect or multi-tab access
- Reconnect counter shows 0 on fresh boot, increments only on genuine drops
- RSSI Stability sparkline shows low flat line under normal operation; spikes visible when camera is moved to edge of range
- PDS History sparkline shows gradual climb during `validate.py rssi` run, returns to baseline on recovery
- Device card reconnect count and viewer count numbers unchanged — still present and accurate

---

## Milestone 14 — DHCP + mDNS: Cross-Platform Network Compatibility

### Problem

Both ESP nodes used static IPs (`192.168.137.10` / `192.168.137.20`) tied to the Windows laptop hotspot subnet. This caused complete connectivity failure on any other network:

**Fedora hotspot (`wlp3s0`):** NetworkManager assigns `10.42.0.1/24` to the hotspot interface. The ESP associates with the AP successfully (WiFi and IP are independent layers) but then self-assigns `192.168.137.x` — a subnet the laptop has no route to. `arp -n` showed no ESP entry. `ping 192.168.137.10` returned "Network unreachable". Neither the dashboard nor the camera stream were reachable. `curl` produced the same result.

Investigation steps:
- Confirmed `ap_isolate` was already `0` in the NM hotspot profile — not the cause
- `sudo firewall-cmd --zone=nm-shared --list-all` revealed `forward: no` and a catch-all `reject` rule at priority 32767 — port 80 and 5000 not in the allowed list
- Root cause confirmed: **subnet mismatch**, not firewall. The firewall would have been a secondary issue but the primary block was the ESP being on the wrong subnet entirely

**Phone hotspot (Android, 2.4GHz):** Same subnet mismatch plus client isolation enabled by default on most Android hotspots — double-blocked.

**WSL:** Same issue as Fedora; WSL routes through the Windows network stack but the static IP approach is equally brittle there.

**Why Windows worked:** Windows laptop hotspot defaults to `192.168.137.x`, which happens to match the hardcoded static IPs. This was never intentional alignment — it was coincidence.

### Fix — both nodes

#### ESP32-CAM (`firmware/esp32cam/`)

**`wifi_manager.cpp`:**
- Removed `WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS1)` call — DHCP now used by default
- Added `#include <ESPmDNS.h>`; `MDNS.begin("tinyguard-cam")` called after `GOT_IP`
- `MDNS.addService("http", "tcp", 80)` advertises the stream endpoint for network scanners
- Added `wifi_manager_resolve_monitor_ip()` — called once by `heartbeat_service_init()` after WiFi is up. Tries `MDNS.queryHost("tinyguard-monitor")` up to 5 times with 1s delays, writes result into `s_monitor_ip[16]`. Falls back to `MONITOR_FALLBACK_IP` compile-time literal if mDNS resolution fails (empty string = disable heartbeats gracefully)
- Added `wifi_manager_get_monitor_ip()` — returns resolved address string, consumed by `heartbeat_service_tick()`

**`wifi_manager.h`:**
- Added declarations for `wifi_manager_resolve_monitor_ip()` and `wifi_manager_get_monitor_ip()`

**`heartbeat_service.cpp`:**
- Removed hardcoded `MONITOR_IP = "192.168.137.20"`
- `heartbeat_service_init()` now calls `wifi_manager_resolve_monitor_ip()` and logs the resolved address
- `heartbeat_service_tick()` reads `wifi_manager_get_monitor_ip()` each tick; skips send if empty (monitor not yet resolved or not present on network)

#### Monitor (`firmware/monitor/`)

**`wifi_manager.c`:**
- Removed `esp_netif_dhcpc_stop()` and `esp_netif_set_ip_info()` calls — DHCP client left running
- Removed `STATIC_IP`, `STATIC_GW`, `STATIC_MASK` defines
- Added `mdns_init()`, `mdns_hostname_set("tinyguard-monitor")`, `mdns_instance_name_set()`, `mdns_service_add("_http", "_tcp", 80)` — called after `WIFI_CONNECTED_BIT` is set
- Serial now logs DHCP-assigned IP from `IP_EVENT_STA_GOT_IP` event data (unchanged) plus mDNS hostname

**`wifi_manager.h`:** No API change — `wifi_manager_init()` signature unchanged.

**`main/CMakeLists.txt`:** `mdns` must be added to `REQUIRES`. Omitting it produces a clean build but `mdns_init()` links to nothing and silently fails at runtime.

### Additional Fedora firewall fix

Even with DHCP resolving correctly, `nm-shared` zone blocks port 80 (TCP) and port 5000 (UDP) by default with a catch-all reject rule. Required one-time fix on the Fedora host:

```bash
sudo firewall-cmd --zone=nm-shared --add-port=80/tcp --permanent
sudo firewall-cmd --zone=nm-shared --add-port=5000/udp --permanent
sudo firewall-cmd --zone=nm-shared --add-forward --permanent
sudo firewall-cmd --reload
```

This is a host-side fix — not in firmware. Documented here for reproducibility when setting up on a new Fedora machine.

### Result

- Both nodes acquire DHCP addresses from whatever hotspot they connect to
- Dashboard reachable at `http://tinyguard-monitor.local/` on Windows, Fedora, and Android hotspots without any firmware change
- Camera stream reachable at `http://tinyguard-cam.local/stream`
- Hotspot connected-devices list now shows both ESPs with their DHCP-assigned IPs (previously showed "IP not available" because the self-assigned static IP wasn't visible to the DHCP server)
- Heartbeat routing is fully dynamic: camera resolves monitor hostname at boot, no hardcoded target IP
- Monitor does not need to know the camera's IP — UDP receiver listens on `0.0.0.0:5000` and accepts packets from any source (unchanged)

### Notes

- mDNS resolution on first boot can take 3–5 seconds. `wifi_manager_resolve_monitor_ip()` retries 5 times with 1s intervals before giving up. If the monitor isn't on the network yet, heartbeats are silently skipped until the camera reboots or the monitor comes online and the camera is reflashed. A future improvement would be periodic re-resolution in `heartbeat_service_tick()` when `s_monitor_ip` is empty.
- mDNS `.local` resolution requires the host OS to support mDNS (Avahi on Linux, Bonjour on macOS/Windows 10+). All three target platforms support it. WSL2 may require Avahi running in the WSL instance (`sudo systemctl start avahi-daemon`) if `.local` names don't resolve.

### Build error — mDNS component not found

**Symptom:** CMake configure step failed with:

```
HINT: The component 'mdns' could not be found. [...] the component has been
moved to the IDF component manager [...]
```

**Root cause:** In newer ESP-IDF versions (v5.x), `mdns` was removed from the built-in component set and moved to the IDF Component Manager registry (`components.espressif.com`). Adding it to `REQUIRES` in `CMakeLists.txt` is necessary but no longer sufficient — the component must also be explicitly fetched via the component manager before it is available to the build system.

**Fix:**

```bash
cd firmware/monitor
idf.py add-dependency "espressif/mdns"
```

This creates/updates `main/idf_component.yml` with the dependency entry and downloads the component into the build tree on the next `idf.py build`. The `REQUIRES mdns` line in `CMakeLists.txt` remains unchanged — both are needed.

### Build error — missing PRIV_REQUIRES for esp_wifi and friends

**Symptom:** Compilation failed immediately after the mDNS fix with:

```
Compilation failed because wifi_manager.c includes esp_wifi.h, provided by
esp_wifi component(s). However, esp_wifi component(s) is not in the
requirements list of "main".
To fix this, add esp_wifi to PRIV_REQUIRES list of idf_component_register.
```

**Root cause:** ESP-IDF v5 enforces strict component dependency declarations. Any component whose headers are `#include`d in a source file must be explicitly listed — either in `REQUIRES` (if the header is also exposed in a public `.h`) or `PRIV_REQUIRES` (if only used internally in `.c` files). The previous `CMakeLists.txt` had no `PRIV_REQUIRES` at all, which worked in older IDF versions where transitive dependencies were pulled in implicitly. `esp_wifi`, `nvs_flash`, `esp_netif`, and `esp_event` are all used directly in `wifi_manager.c` but none are exposed in `wifi_manager.h`, so all four belong in `PRIV_REQUIRES`.

**Fix:** Added `PRIV_REQUIRES` to `idf_component_register` in `main/CMakeLists.txt`:

```cmake
REQUIRES mdns
PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event
```

All four added at once rather than one at a time — each missing entry would have produced the same error on the next build cycle.

### Build error — missing PRIV_REQUIRES for esp_driver_gpio, esp_http_server, lwip

**Symptom:** Build failed again after the esp_wifi fix:

```
Compilation failed because udp_receiver.c includes driver/gpio.h, provided by
esp_driver_gpio component(s). However, esp_driver_gpio component(s) is not in
the requirements list of "main".
```

**Root cause:** Same class of error as above — all source files were audited for angle-bracket includes to find remaining undeclared dependencies: `driver/gpio.h` (`udp_receiver.c`) → `esp_driver_gpio`; `esp_http_server.h` (`dashboard_server.c`) → `esp_http_server`; `lwip/sockets.h` and `lwip/netdb.h` (`udp_receiver.c`) → `lwip`. None exposed in public headers, so all belong in `PRIV_REQUIRES`.

**Fix:** Final `CMakeLists.txt` `PRIV_REQUIRES` list:

```cmake
idf_component_register(
    SRCS ...
    INCLUDE_DIRS "."
    REQUIRES mdns
    PRIV_REQUIRES
        esp_wifi
        nvs_flash
        esp_netif
        esp_event
        esp_driver_gpio
        esp_http_server
        lwip
)
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
```

All remaining undeclared dependencies resolved in one pass by scanning all source files for ESP-IDF includes before building again.

### Issue — dashboard and camera log wrong URLs after mDNS migration

**Symptom:** Serial output showed `http://tinyguard-monitor.local/` and `http://tinyguard-cam.local/stream`. Opening these in a browser on Fedora produced no response — the `.local` mDNS resolution was unreliable. However, navigating to the DHCP IP printed earlier in the boot log (`10.42.0.178`) worked correctly.

**Root cause:** mDNS `.local` name resolution requires the host OS to have an mDNS resolver active. On Fedora this is Avahi (`avahi-daemon`), which may not be running or may not be resolving `.local` names from the hotspot interface. The server itself was working fine — only the logged URL was misleading.

**Fix — `dashboard_server.c`:**
- Added `#include "esp_netif.h"`
- `dashboard_server_start()` now reads the actual DHCP IP via `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")` + `esp_netif_get_ip_info()` and logs it directly using `IPSTR` / `IP2STR`
- mDNS hostname logged as a secondary line so it's still visible when it works
- Falls back to hostname-only log if `esp_netif_get_ip_info()` fails

**Fix — `camera_server.cpp`:**
- `handle_root()` now builds the status string dynamically using `WiFi.localIP().toString()` — the `GET /` response shows the actual IP alongside the mDNS hostname
- `camera_server_init()` logs both the IP and the mDNS hostname after server start

Serial output now:
```
I (19662) Dashboard: Dashboard : http://10.42.0.178/
I (19663) Dashboard: Status    : http://10.42.0.178/status
I (19664) Dashboard: mDNS also : http://tinyguard-monitor.local/
```
```
[Camera] Stream : http://10.42.0.235/stream
[Camera] mDNS  : http://tinyguard-cam.local/stream
```

The DHCP IP is always the primary URL. mDNS remains functional where supported.

---

## Milestone 15 — Camera Stream Stability, Brownout Recovery, BSD Socket Heartbeat

### Image orientation

Camera mounted inverted (slotted between PCB and ESP-CAM module). Applied `set_hmirror(1)` and `set_vflip(1)` via OV3660 sensor register API after `esp_camera_init()`. Stream appears correctly oriented in browser without any client-side transform.

### `/view` as sole browser entry point — `/stream` removed as public route

Initially both `/view` (HTML wrapper) and `/stream` (raw MJPEG) were exposed. After testing, `/view` was noticeably smoother than `/stream` on both Firefox and Chrome/Brave — the `<img>` tag approach lets the browser handle multipart buffering more gracefully than direct stream navigation. `/stream` is still registered internally (the `/view` page's `<img src>` points to it) but is no longer the intended user-facing URL. `handle_root()` and all serial log lines updated to reference only `/view`.

### Brownout logging and threshold (`esp32cam.ino`)

The AI Thinker ESP32-CAM draws up to ~300 mA during WiFi TX bursts. On resistive USB cables or current-limited laptop ports, voltage sags cause brownout resets that appear as silent reboots or stream cutoffs.

Added to `esp32cam.ino`:
- `log_reset_reason()` — called first on every boot, logs `esp_reset_reason()` to serial. Brownout resets now print `[Boot] Reset: BROWNOUT DETECTED` with hardware fix advice instead of appearing as silent reboots.
- `brownout_threshold_set(1)` — lowers the brownout detector trip point from default level 3 (~2.68V) to level 1 (~2.43V) via direct `RTC_CNTL_BROWN_OUT_REG` write. Gives ~250mV more headroom before reset fires. Chip remains protected.

Recommended hardware fix: 100µF electrolytic capacitor across 5V/GND pins, dedicated USB charger (1A+) or USB-C breakout board instead of laptop USB port.

### Heartbeat task isolation and BSD socket fix

**Problem:** Stream write failed exactly every 10 seconds — precisely on the heartbeat interval. Root cause: `WiFiUDP` (Arduino) and `WiFiClient` (Arduino WebServer) share internal lwIP state. Concurrent UDP send from the heartbeat and TCP write from the streamer corrupted socket state regardless of which core each ran on.

**Fix:** Replaced `WiFiUDP` entirely in `heartbeat_service.cpp` with raw BSD sockets (`lwip/sockets.h`). BSD sockets are re-entrant and do not share state with the Arduino `WiFiClient` layer — TCP and UDP can now run simultaneously on different cores without interference.

`heartbeat_service_init()` opens a connected UDP socket (`socket()` → `connect()`) once after monitor IP is resolved. `heartbeat_service_tick()` uses `send()` — no address needed per call. On send failure the socket is closed and reopened next tick.

Heartbeat moved from `loop()` into a dedicated `xTaskCreatePinnedToCore` task on core 0. HTTP server and streamer tasks remain on core 1. `loop()` is now empty (`vTaskDelay(1000ms)`).

### DHCP monitor IP resolution

`wifi_manager.cpp` resolves `tinyguard-monitor.local` via `MDNS.queryHost()` at boot (5 retries, 1s apart). Resolved IP stored in `s_monitor_ip[16]`. `MONITOR_IP` compile-time literal acts as override when set; left empty (`""`) to use mDNS by default. `heartbeat_service_tick()` retries resolution every 30s when `s_monitor_ip` is empty — covers the case where the monitor boots after the camera.

### Board manager warning

**The esp32cam firmware must be flashed using the `esp32` board package by Espressif with board set to `AI Thinker ESP32-CAM`.** Third-party board managers (e.g. ESP32-BluePad32) use a different WiFi/lwIP stack underneath and cause unpredictable stream failures, socket corruption, and build incompatibilities. Do not use them for this project.
