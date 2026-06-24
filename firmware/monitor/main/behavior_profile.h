#pragma once

/*
 * behavior_profile.h — TinyGuard Monitor (Phase 2)
 *
 * Long-term behavioral profiling using Exponential Moving Average (EMA).
 *
 * PURPOSE
 * -------
 * stats_engine maintains a 60-sample (~10 min) rolling window optimised
 * for short-term anomaly detection via z-score. That window moves with
 * the device's recent behavior, which means slow drifts — gradual shifts
 * in mean behavior over hours — are invisible: the baseline drifts with
 * the signal, and individual z-scores stay low throughout.
 *
 * behavior_profile adds a second, longer memory. A new sample has small
 * weight (α = 0.005) relative to accumulated history, giving the profile
 * an effective memory of ~70 minutes. The profile resists short spikes
 * (handled by Phase 1) and tracks the slow center-of-mass of each metric.
 *
 * When the short-term window mean drifts away from the long-term profile,
 * something has shifted in device behavior even though no single sample
 * is an outlier. This is the class of event Phase 2 is designed to catch.
 *
 * ALGORITHM
 * ---------
 * Finch (2009) EMA with online variance:
 *
 *   diff     = x - ema_mean
 *   ema_mean = ema_mean + α × diff
 *   ema_var  = (1-α) × (ema_var + α × diff²)
 *
 * Numerically stable, zero-buffer, single-pass. ema_var converges to the
 * variance of the EMA-weighted distribution.
 *
 * SEEDING — CRITICAL DESIGN DECISION
 * -----------------------------------
 * Zero-initialising ema_mean causes a large startup bias for non-zero-
 * centered metrics (RSSI ≈ -55 dBm is a clear example). Simulation shows
 * that starting from 0 with α=0.005, the mean still has a 22 dBm bias
 * after 180 samples — large enough to produce false z-scores.
 *
 * Resolution: ema_mean is seeded from the Phase 1 short-term baseline
 * mean at the moment learning completes. This is called via
 * behavior_profile_seed() from stats_engine's learning-complete path
 * (triggered by udp_receiver).
 *
 * Seeding the mean but not the variance is intentional: the variance
 * starts at 0 and builds up from observed spread, which is correct —
 * we want the long-term variance to reflect actual long-term variability,
 * not inherit a 5-minute window's estimate.
 *
 * With seeded mean, ema_var stabilises meaningfully by n=90 samples.
 * MIN_SAMPLES = 120 (20 min post-learning) is conservative and safe.
 *
 * α SELECTION
 * -----------
 * α = BEHAVIOR_PROFILE_ALPHA = 0.005. At 10s heartbeat interval:
 *   half-life  = ln(2)/α ≈ 139 samples ≈ 23 min
 *   95% horizon = 3× half-life ≈ 416 samples ≈ 69 min
 *
 * THREAD SAFETY
 * -------------
 * Internal mutex. All public functions are safe to call from any task.
 *
 * MEMORY
 * ------
 * sizeof(long_term_stat_t) = float(4) + float(4) + uint32_t(4) + bool(1) + 3 pad
 *                          = 16 bytes
 * Four metric profiles     = 64 bytes
 * Module state wrapper     = ~8 bytes (mutex handle + padding)
 * Total static RAM         = ~72 bytes
 *
 * INTEGRATION
 * -----------
 * Init:    behavior_profile_init()   — called from app_main()
 * Seed:    behavior_profile_seed()   — called from udp_receiver when
 *                                      stats_engine transitions out of
 *                                      learning (snap.learning goes false
 *                                      for the first time)
 * Update:  behavior_profile_update() — called from udp_receiver each
 *                                      heartbeat, after stats_engine_update()
 * Read:    behavior_profile_get_snapshot() — called by fingerprint_engine
 */

#include "stats_engine.h"
#include <stdint.h>
#include <stdbool.h>

/* ── tunables ─────────────────────────────────────────────────────────── */

/*
 * EMA decay factor. Controls the long-term memory horizon.
 * At 10s/sample: half-life ≈ 139 samples ≈ 23 min; 95% horizon ≈ 69 min.
 */
#define BEHAVIOR_PROFILE_ALPHA          0.005f

/*
 * Samples required (post-seed) before a profile is considered ready.
 * 120 × 10s = 20 minutes after the Phase 1 learning phase ends.
 * With a seeded ema_mean, ema_var stabilises well before this point.
 */
#define BEHAVIOR_PROFILE_MIN_SAMPLES    120

/*
 * Z-score threshold for long-term divergence detection.
 * Set lower than Phase 1 (3.0) because the long-term profile is comparing
 * the short-term window mean (a stable estimator) against the profile,
 * not a single noisy sample. A 2.5σ shift in the window mean is significant.
 */
#define BEHAVIOR_PROFILE_ZSCORE_THRESH  2.5f

/* ── data structures ─────────────────────────────────────────────────── */

/* EMA state for one metric. */
typedef struct {
    float    ema_mean;    /* exponentially weighted mean (seeded from Phase 1) */
    float    ema_var;     /* exponentially weighted variance                   */
    uint32_t n;           /* samples ingested since seed                       */
    bool     seeded;      /* true after behavior_profile_seed() sets ema_mean  */
    bool     ready;       /* true after MIN_SAMPLES post-seed                  */
} long_term_stat_t;

/* Snapshot of all four profiles. Consumed by fingerprint_engine. */
typedef struct {
    long_term_stat_t rssi;
    long_term_stat_t heartbeat_interval_ms;
    long_term_stat_t reconnect_rate;
    long_term_stat_t viewer_count;

    /* True only when every profile has reached MIN_SAMPLES post-seed. */
    bool all_ready;
} behavior_profile_snapshot_t;

/* ── public API ──────────────────────────────────────────────────────── */

/* Call once from app_main() before the UDP receiver task starts. */
void behavior_profile_init(void);

/*
 * Seed the long-term profile means from the Phase 1 short-term baseline.
 *
 * Called exactly once — when stats_engine transitions out of learning
 * for the first time (the heartbeat at which snap->learning flips false).
 * udp_receiver.c tracks this transition and calls this function.
 *
 * Seeding ema_mean eliminates the zero-start bias that would otherwise
 * require ~720 samples (~2 hours) for the EMA to converge on metrics
 * like RSSI that are far from zero.
 *
 * After seeding, variance accumulation begins from 0 and builds
 * organically from observed spread — intentionally not inherited from
 * the Phase 1 rolling-window variance.
 */
void behavior_profile_seed(const stats_snapshot_t *snap);

/*
 * Ingest one heartbeat's statistics into the long-term profiles.
 *
 * Suppressed during Phase 1 learning (snap->learning == true) and before
 * seed has been applied (profiles not yet seeded). Each call updates all
 * four EMA accumulators using snap->{metric}.latest.
 */
void behavior_profile_update(const stats_snapshot_t *snap);

/* Return a copy of current profile state. Mutex-safe. */
behavior_profile_snapshot_t behavior_profile_get_snapshot(void);

/*
 * Compute z-score of `current` against a long-term profile.
 * Returns 0.0f when the profile is not ready or variance is negligible.
 *
 *   z = (current - ema_mean) / sqrt(ema_var)
 *
 * Used by fingerprint_engine to score long-term behavioral deviation.
 */
float behavior_profile_zscore(const long_term_stat_t *profile, float current);
