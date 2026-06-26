#pragma once

/*
 * fingerprint_engine.h — TinyGuard Monitor (Phase 2)
 *
 * Behavioral fingerprinting and Profile Divergence Score (PDS).
 *
 * PURPOSE
 * -------
 * behavior_profile, correlation_tracker, and session_tracker each model
 * one dimension of normal camera behavior. fingerprint_engine combines
 * all three into a single scalar: the Profile Divergence Score (PDS).
 *
 * The PDS answers: "How far has this camera's current behavior drifted
 * from its learned fingerprint?" It rises gradually as behavior shifts
 * and falls as behavior normalizes — more useful for situational awareness
 * than individual module binary alerts.
 *
 * PROFILE DIVERGENCE SCORE (PDS)
 * --------------------------------
 * Range: 0–100 (uint8_t).
 *
 * Thresholds:
 *   0–24   Normal      — behavior within expected range
 *   25–49  Elevated    — one or more signals drifting; worth watching
 *   50–74  Suspicious  — significant divergence across multiple signals
 *   75–100 Critical    — severe multi-signal divergence; likely anomalous
 *
 * Three weighted components:
 *
 *   Metric Drift (D)       weight 40%
 *     Source: behavior_profile — z-scores for RSSI, HB interval,
 *             reconnect rate, viewer count via behavior_profile_zscore().
 *     Aggregation: maximum |z| across all ready channels.
 *
 *   Correlation Drift (C)  weight 35%
 *     Source: correlation_tracker — zscore field per pair snapshot.
 *     Aggregation: maximum |z| across all ready pairs.
 *
 *   Session Drift (S)      weight 25%
 *     Source: session_tracker — session count deviation and CV signals.
 *     When session_tracker.ready == false, this component is zero and
 *     its weight redistributes proportionally to D and C.
 *
 * Sub-score mapping (linear ramp, deterministic, no exp()):
 *   sub_score = clamp(|z| / FINGERPRINT_Z_SCALE * 100, 0, 100)
 *   At z=3: sub_score=50. At z=FINGERPRINT_Z_SCALE: sub_score=100.
 *
 * PDS = clamp(D*0.40 + C*0.35 + S*0.25, 0, 100)
 *
 * When session not ready:
 *   PDS = clamp((D*0.40 + C*0.35) / 0.75, 0, 100)
 *
 * EXPLAINABILITY
 * --------------
 * Every PDS value is fully decomposable. fingerprint_snapshot_t exposes
 * per-component sub-scores and driving z-scores for dashboard display.
 * No black-box computation anywhere in the pipeline.
 *
 * ALERT GENERATION
 * ----------------
 * Fires after FINGERPRINT_CONSECUTIVE_THRESH consecutive evaluations
 * above threshold. Clears after the same number below threshold.
 *
 *   ALERT_TYPE_PDS_ELEVATED  — PDS >= PDS_THRESH_ELEVATED   WARNING
 *   ALERT_TYPE_PDS_CRITICAL  — PDS >= PDS_THRESH_CRITICAL   CRITICAL
 *
 * READINESS
 * ---------
 * Minimum gate: behavior_profile.all_ready. Below this, PDS = 0 and
 * snapshot.ready = false. No alerts raised.
 *
 * correlation_tracker and session_tracker readiness is per-component:
 * a not-ready component contributes zero with weight redistribution.
 * The engine produces a partial but useful PDS from the moment
 * behavior_profile.all_ready becomes true.
 *
 * MEMORY
 * ------
 *   fingerprint_state_t:    ~48 bytes static
 *   No heap allocation.
 *
 * THREAD SAFETY
 * -------------
 * fingerprint_engine_update() called from udp_rx_task only.
 * fingerprint_engine_get_snapshot() may be called from dashboard_server.
 * Internal mutex protects snapshot reads.
 *
 * INTEGRATION
 * -----------
 * Init:    fingerprint_engine_init()     — app_main(), after all upstream inits
 * Update:  fingerprint_engine_update()   — udp_rx_task, after session_tracker_update()
 * Read:    fingerprint_engine_get_snapshot() — dashboard_server
 *
 * fingerprint_engine_update() is the terminal pipeline consumer. It calls
 * get_snapshot() on all upstream modules internally and requires no parameters.
 */

#include "behavior_profile.h"
#include "correlation_tracker.h"
#include "session_tracker.h"
#include <stdint.h>
#include <stdbool.h>

/* ── tunables ─────────────────────────────────────────────────────────── */

/*
 * Z-score at which a component sub-score saturates at 100.
 * z = Z_SCALE/2 → sub_score = 50.
 * Default 6.0: z=3 → 50, z=6 → 100.
 */
#define FINGERPRINT_Z_SCALE             6.0f

/* PDS threshold for ELEVATED alert (WARNING). */
#define PDS_THRESH_ELEVATED             25

/* PDS threshold for CRITICAL alert. */
#define PDS_THRESH_CRITICAL             75

/*
 * Consecutive evaluations above/below threshold before alert fires/clears.
 * Mirrors ANOMALY_CONSECUTIVE_THRESH and CORRELATION_CONSECUTIVE_THRESH.
 */
#define FINGERPRINT_CONSECUTIVE_THRESH  3

/* ── data structures ─────────────────────────────────────────────────── */

/* Per-component breakdown — for dashboard explainability. */
typedef struct {
    uint8_t sub_score;    /* 0–100 before weighting                        */
    float   max_zscore;   /* z-score that drove this sub-score             */
    bool    ready;        /* component inputs were ready for this eval     */
} pds_component_t;

/* Full snapshot — returned to dashboard_server. */
typedef struct {
    uint8_t         pds;                 /* Profile Divergence Score 0–100  */
    pds_component_t metric_drift;        /* D component (behavior_profile)  */
    pds_component_t correlation_drift;   /* C component (corr_tracker)      */
    pds_component_t session_drift;       /* S component (session_tracker)   */
    bool            alert_elevated;      /* PDS >= PDS_THRESH_ELEVATED      */
    bool            alert_critical;      /* PDS >= PDS_THRESH_CRITICAL      */
    bool            ready;               /* bp.all_ready satisfied          */
} fingerprint_snapshot_t;

/* ── public API ──────────────────────────────────────────────────────── */

/* Call once from app_main() after all upstream module inits. */
void fingerprint_engine_init(void);

/*
 * Compute PDS from current upstream module state.
 * Called from udp_rx_task after session_tracker_update().
 * Internally calls get_snapshot() on behavior_profile, stats_engine,
 * correlation_tracker, and session_tracker.
 */
void fingerprint_engine_update(void);

/*
 * Return a copy of the current fingerprint state. Mutex-safe.
 * Returns pds=0 and ready=false until behavior_profile.all_ready.
 */
fingerprint_snapshot_t fingerprint_engine_get_snapshot(void);
