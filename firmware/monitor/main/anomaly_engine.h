#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * anomaly_engine.h — TinyGuard Monitor
 *
 * Z-score anomaly detection against rolling baselines from stats_engine.
 *
 * Method:
 *   z = (observed - mean) / stddev
 *   If |z| > threshold AND stddev > MIN_STDDEV: anomaly flagged.
 *
 * Consecutive-sample guard:
 *   A single outlier sample is not enough to raise an alert. The spec
 *   requires 3 consecutive anomalous samples for RSSI. We apply the same
 *   guard to all metrics to reduce false positives from transient spikes.
 *
 * Heartbeat timeout:
 *   Handled separately from z-score — if no heartbeat arrives within
 *   HEARTBEAT_TIMEOUT_MS, a CRITICAL alert is raised. This is checked
 *   in a dedicated watchdog task rather than in the receive path.
 *
 * Detection is a no-op during the learning phase (stats_engine_is_learning()).
 */

/* ── tunables ─────────────────────────────────────────────────────────── */

/* Z-score threshold (spec default: 3.0, configurable) */
#define ANOMALY_ZSCORE_THRESHOLD    3.0f

/*
 * Minimum stddev before z-score is meaningful.
 * If stddev is near zero the metric is perfectly stable — z-scores will
 * explode on the first deviation. Skip detection until we have real spread.
 */
#define ANOMALY_MIN_STDDEV          0.5f

/*
 * Consecutive anomalous samples required before an alert fires.
 * Spec requires 3 for RSSI; we use 3 for all metrics.
 */
#define ANOMALY_CONSECUTIVE_THRESH  3

/*
 * Heartbeat timeout: 30s = 3 missed 10s heartbeats (from spec).
 */
#define HEARTBEAT_TIMEOUT_MS        30000

/* ─────────────────────────────────────────────────────────────────────── */

void anomaly_engine_init(void);

/*
 * Run all metric checks against the current stats snapshot.
 * Call after every stats_engine_update() — i.e. from udp_receiver
 * after each heartbeat is processed.
 */
void anomaly_engine_evaluate(void);

/*
 * Called by the heartbeat watchdog task when a timeout is detected.
 * Raises CRITICAL alert.
 */
void anomaly_engine_heartbeat_timeout(void);

/*
 * Notify the engine that a heartbeat arrived (resets the timeout clock).
 * Called from udp_receiver on each successful packet.
 */
void anomaly_engine_heartbeat_received(void);
