#pragma once

/*
 * correlation_tracker.h — TinyGuard Monitor (Phase 2)
 *
 * Rolling Pearson correlation tracking between metric pairs.
 * Detects abnormal relationships between metrics even when individual
 * metrics remain within their own expected ranges.
 *
 * PAIRS TRACKED
 * -------------
 * Three pairs selected for physical meaningfulness:
 *
 *   RSSI ↔ reconnect_rate
 *     Signal degradation normally correlates with reconnect frequency.
 *     A break in this relationship can indicate spoofed signal metrics
 *     or an attack that forces reconnects without affecting RSSI.
 *
 *   RSSI ↔ heartbeat_interval
 *     Weak signal causes transmission stress, reflected in irregular
 *     inter-packet timing. If RSSI degrades without interval change,
 *     the camera may not be the source of the signal measurements.
 *     Note: this pair lags Pairs 0 and 2 in reaching readiness because
 *     it skips samples until stats_engine has at least 2 HB interval
 *     observations. This is correct — feeding an undefined HB interval
 *     (before any delta can be computed) would corrupt the buffer.
 *
 *   stream_active ↔ viewer_count
 *     A stream being active without viewers, or viewers without stream,
 *     is anomalous. This pair catches unauthorized stream access where
 *     an attacker suppresses viewer reporting, or viewer injection where
 *     the stream is not actually active.
 *     stream_active is the real boolean field from the heartbeat packet,
 *     passed directly from udp_receiver — NOT derived from viewer_count.
 *
 * Reconnect_rate ↔ viewer_count was explicitly excluded: no strong
 * physical relationship exists between viewer activity and reconnect
 * behavior; the expected baseline converges toward noise and increases
 * false-positive risk without meaningful detection value.
 *
 * ALGORITHM
 * ---------
 * Rolling two-pass Pearson r over STATS_WINDOW_SIZE samples per pair.
 * Recomputed from scratch on each heartbeat (same rationale as Welford
 * eviction in stats_engine — existing buffers make incremental removal
 * equivalent in cost to a full pass).
 *
 * A baseline EMA over computed r values learns the expected correlation
 * for each pair. Z-score alerts when current r deviates significantly
 * from the long-term expected r.
 *
 * READINESS — THREE LAYERS
 * ------------------------
 * 1. Phase 1 learning complete      (snap->learning == false)
 * 2. behavior_profile all_ready     (bp->all_ready == true)
 *    — ensures EMA variances are mature before correlation is trusted
 * 3. Per-pair buffer has >= CORRELATION_MIN_SAMPLES AND the first valid
 *    Pearson r has seeded the EMA baseline (baseline_seeded == true)
 *
 * Readiness is tracked and enforced per pair. Each pair begins emitting
 * alerts independently as soon as its own buffer satisfies layer 3.
 * Pairs do not wait for each other.
 *
 * corr_pair_snapshot_t.ready  — true when that pair satisfies layer 3
 * correlation_snapshot_t.ready — true when ALL pairs satisfy layer 3
 *
 * Layers 1 and 2 are global gates: no pair accumulates samples or emits
 * alerts until both are satisfied.
 *
 * MEMORY
 * ------
 * Per correlation_pair_t:
 *   buf_x[60] + buf_y[60]      480 bytes (two float[60] sample buffers)
 *   head, n                      8 bytes
 *   current_r, ema_r, ema_r_var 12 bytes
 *   samples_total                4 bytes
 *   flags + counters            ~8 bytes + padding
 *   Total per pair             ~514 bytes
 *
 * Three pairs:                ~1542 bytes
 * Module wrapper (mutex):       ~8 bytes
 * Total static RAM:          ~1550 bytes
 *
 * No heap allocation. No dynamic containers. Fixed-size structures only.
 *
 * THREAD SAFETY
 * -------------
 * correlation_tracker_update() is called from udp_rx_task only.
 * correlation_tracker_get_snapshot() may be called from the HTTP handler
 * task (dashboard_server). An internal mutex protects the snapshot copy.
 * The Pearson computation loop runs outside the mutex; only the result
 * write and EMA update are protected — matching stats_engine's pattern.
 *
 * SEPARATION OF CONCERNS
 * ----------------------
 * This module tracks and learns metric relationships only.
 * It does not duplicate anomaly_engine state or logic.
 * Alert generation uses alert_manager interfaces exclusively.
 * Cooldown and consecutive-sample guards mirror anomaly_engine's pattern
 * but operate on independent state — no shared fields.
 *
 * INTEGRATION
 * -----------
 * Init:    correlation_tracker_init()       — called from app_main()
 * Update:  correlation_tracker_update()     — called from udp_rx_task,
 *                                             after behavior_profile_update()
 * Read:    correlation_tracker_get_snapshot() — called by dashboard_server
 *
 * The stream_active parameter in correlation_tracker_update() must be the
 * raw boolean from the parsed heartbeat packet — do not derive it from
 * viewer_count. udp_receiver.c is the sole call site.
 */

#include "stats_engine.h"
#include "behavior_profile.h"
#include <stdint.h>
#include <stdbool.h>

/* ── tunables ─────────────────────────────────────────────────────────── */

/*
 * Minimum samples in a pair's rolling window before Pearson r is trusted.
 * 30 × 10s = 5 minutes of post-all_ready data.
 * Below 30, Pearson r has high variance and can produce misleading readings.
 */
#define CORRELATION_MIN_SAMPLES     30

/*
 * EMA decay factor for the r baseline.
 * Matches BEHAVIOR_PROFILE_ALPHA for consistency.
 * At 10s/sample: half-life ≈ 139 samples ≈ 23 min.
 */
#define CORRELATION_ALPHA           0.005f

/*
 * Z-score threshold for correlation anomaly alerts.
 * Applied to (current_r - ema_r) / sqrt(ema_r_var).
 * Set equal to Phase 1 threshold; correlation z-scores compare a
 * stable rolling r against a learned baseline, not a single noisy sample.
 */
#define CORRELATION_ZSCORE_THRESH   3.0f

/*
 * Variance floor for ema_r_var.
 * If ema_r_var < 0.0025 (ema_r_stddev < 0.05), suppress z-score.
 * Prevents division-by-near-zero when r has been extremely stable.
 */
#define CORRELATION_VAR_FLOOR       0.0025f

/*
 * Consecutive anomalous/normal samples required to fire/clear an alert.
 * Mirrors ANOMALY_CONSECUTIVE_THRESH from anomaly_engine.
 */
#define CORRELATION_CONSECUTIVE_THRESH  3

/* ── data structures ─────────────────────────────────────────────────── */

/* Pair identifiers — used in alert messages and dashboard labels. */
typedef enum {
    CORR_PAIR_RSSI_RECONNECT  = 0,   /* RSSI ↔ reconnect_rate   */
    CORR_PAIR_RSSI_HB_INTERVAL = 1,  /* RSSI ↔ HB interval      */
    CORR_PAIR_STREAM_VIEWERS   = 2,  /* stream_active ↔ viewers  */
    CORR_PAIR_COUNT            = 3,
} corr_pair_id_t;

/* Per-pair snapshot — read-only view for consumers. */
typedef struct {
    float    current_r;       /* Pearson r from most recent window          */
    float    ema_r;           /* EMA baseline (learned expected r)           */
    float    ema_r_stddev;    /* sqrt(ema_r_var) — for dashboard display     */
    float    zscore;          /* (current_r - ema_r) / ema_r_stddev         */
    bool     ready;           /* layer 3 satisfied for this pair             */
    bool     alert_active;    /* alert currently fired for this pair         */
} corr_pair_snapshot_t;

/* Full module snapshot — returned to dashboard_server and fingerprint_engine. */
typedef struct {
    corr_pair_snapshot_t pairs[CORR_PAIR_COUNT];
    bool                 ready;   /* true when ALL pairs satisfy all 3 layers */
} correlation_snapshot_t;

/* ── public API ──────────────────────────────────────────────────────── */

/* Call once from app_main() after alert_manager_init() and behavior_profile_init(). */
void correlation_tracker_init(void);

/*
 * Ingest one heartbeat. Called from udp_rx_task after behavior_profile_update().
 *
 * snap          — Phase 1 statistics snapshot (taken once in udp_receiver)
 * bp            — Phase 2 behavioral profile snapshot (taken once in udp_receiver)
 * stream_active — raw stream_active boolean from the parsed heartbeat packet
 *
 * stream_active must NOT be derived from viewer_count. It must be the value
 * parsed directly from the heartbeat JSON. This is the only way to detect
 * stream_active=1 with viewer_count=0 (the primary threat case for that pair).
 *
 * Suppressed internally when any of layers 1–2 is unsatisfied.
 * Never calls stats_engine_get_snapshot() or behavior_profile_get_snapshot()
 * internally — snapshot ownership belongs to udp_receiver.
 */
void correlation_tracker_update(const stats_snapshot_t *snap,
                                const behavior_profile_snapshot_t *bp,
                                bool stream_active);

/*
 * Return a copy of current tracker state. Mutex-safe; may be called from
 * any task. Returns a snapshot with ready=false and zero r values if the
 * tracker has not yet satisfied all readiness layers.
 */
correlation_snapshot_t correlation_tracker_get_snapshot(void);
