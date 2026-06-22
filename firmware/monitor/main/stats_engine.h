#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * stats_engine.h — TinyGuard Monitor
 *
 * Rolling-window statistics for each monitored metric.
 *
 * Design:
 *   - Fixed-size circular buffer per metric (no heap allocation)
 *   - Incremental mean and variance using Welford's online algorithm
 *     (numerically stable; avoids recomputing sum over full window on
 *      every sample, which would accumulate float error over time)
 *   - Baseline learning mode: detection is suppressed for the first
 *     LEARNING_DURATION_S seconds. After that, the accumulated stats
 *     are locked as the baseline and z-score detection is enabled.
 *
 * Metrics tracked:
 *   - RSSI (dBm, sampled every heartbeat)
 *   - Heartbeat interval (ms between consecutive heartbeats)
 *   - Reconnect rate (reconnects per rolling hour window)
 *   - Stream viewer count (sampled every heartbeat)
 *
 * Usage:
 *   stats_engine_init();
 *   // called from udp_receiver on each heartbeat:
 *   stats_engine_update(&sample);
 *   // called from detection engine:
 *   stats_snapshot_t snap = stats_engine_get_snapshot();
 */

/* ── tunables ────────────────────────────────────────────────────────── */

/* Learning phase duration in seconds (spec: 5 minutes = 300s) */
#define STATS_LEARNING_DURATION_S   300

/*
 * Rolling window size — number of samples kept per metric.
 * At 1 sample per 10s heartbeat:
 *   30 samples = 5 minutes of history
 *   360 samples = 1 hour of history
 * Using 60 (10 minutes) as a balance between memory and stability.
 * Each window costs: 60 * 4 bytes = 240 bytes.
 */
#define STATS_WINDOW_SIZE           60

/* ──────────────────────────────────────────────────────────────────── */

/* One rolling-window metric */
typedef struct {
    float   buf[STATS_WINDOW_SIZE]; /* circular sample buffer            */
    int     head;                   /* next write index                  */
    int     count;                  /* samples written (capped at WINDOW) */
    double  mean;                   /* Welford running mean              */
    double  M2;                     /* Welford sum of squared deviations */
    uint32_t n;                     /* total samples ever added          */
} rolling_metric_t;

/* Input sample — populated by udp_receiver before calling stats_engine_update */
typedef struct {
    int      rssi;              /* dBm */
    uint32_t heartbeat_rx_ms;   /* esp_log_timestamp() when packet arrived */
    int      reconnects;        /* cumulative reconnect count from camera */
    int      viewer_count;
} stats_sample_t;

/* Per-metric statistics snapshot — output of stats_engine_get_snapshot */
typedef struct {
    float mean;
    float stddev;
    float latest;   /* most recent sample value */
    int   count;    /* samples in window        */
} metric_stats_t;

/* Full snapshot returned to callers */
typedef struct {
    metric_stats_t rssi;
    metric_stats_t heartbeat_interval_ms;
    metric_stats_t reconnect_rate;    /* reconnects per hour, rolling */
    metric_stats_t viewer_count;
    bool           learning;          /* true = still in learning phase */
    uint32_t       learning_remaining_s; /* seconds until learning ends */
    uint32_t       samples_total;     /* total heartbeats processed     */
} stats_snapshot_t;

void             stats_engine_init(void);
void             stats_engine_update(const stats_sample_t *s);
stats_snapshot_t stats_engine_get_snapshot(void);
bool             stats_engine_is_learning(void);
