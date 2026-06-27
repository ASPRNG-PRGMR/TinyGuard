/*
 * correlation_tracker.c — TinyGuard Monitor (Phase 2)
 *
 * Rolling Pearson correlation tracking between metric pairs.
 * See correlation_tracker.h for full design rationale.
 *
 * IMPLEMENTATION NOTES
 * --------------------
 *
 * Pearson computation:
 *   Two-pass over the circular buffers. First pass computes means;
 *   second computes numerator and denominator terms. Degenerate case
 *   (near-zero variance in either buffer) returns current_r = 0.0f and
 *   sets *valid = false — a degenerate r must not feed the EMA baseline.
 *
 * EMA baseline for r:
 *   First valid r seeds ema_r directly (same rationale as behavior_profile
 *   seeding ema_mean from Phase 1 — avoids zero-start bias). Subsequent
 *   samples update via EMA with CORRELATION_ALPHA = 0.005.
 *   baseline_seeded is part of per-pair readiness: a pair whose buffer
 *   has reached CORRELATION_MIN_SAMPLES but has not yet produced a valid
 *   Pearson r (degenerate window) is not considered ready.
 *
 * Mutex scope:
 *   The O(60) Pearson loop runs outside the mutex. The mutex is taken
 *   only to write results (current_r, ema_r, ema_r_var, alert state)
 *   and to copy the snapshot in get_snapshot(). This matches stats_engine's
 *   pattern and keeps the critical section minimal.
 *
 * Alert firing:
 *   Mirrors anomaly_engine's consecutive-sample guard and cooldown pattern
 *   exactly — independent state, same behavior. An alert fires after
 *   CORRELATION_CONSECUTIVE_THRESH consecutive anomalous r values and
 *   clears after the same number of consecutive normal values.
 *
 * stream_active ↔ viewer_count pair:
 *   stream_active is a bool (0 or 1) from the heartbeat packet, passed in
 *   directly as the stream_active parameter. viewer_count is a float from
 *   the stats snapshot. Pearson handles mixed numeric types correctly.
 *   When stream_active never changes in the window (always 0 or always 1),
 *   X variance is zero and the degenerate guard suppresses output — correct
 *   behavior since a constant signal carries no relationship information.
 *
 * RSSI ↔ heartbeat_interval pair:
 *   Samples are skipped when snap->heartbeat_interval_ms.count < 2,
 *   meaning no valid interval delta has been computed yet. This makes Pair 1
 *   reach CORRELATION_MIN_SAMPLES slightly later than Pairs 0 and 2 when
 *   the tracker first starts. This is intentional and correct.
 */

#include "correlation_tracker.h"
#include "alert_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "CorrTracker";

/* Human-readable pair names — used in alert messages and serial log. */
static const char *PAIR_NAME[CORR_PAIR_COUNT] = {
    "RSSI<>Reconnect",
    "RSSI<>HBInterval",
    "Stream<>Viewers",
};

/* ── per-pair internal state ─────────────────────────────────────────── */

typedef struct {
    /* Rolling sample buffers — circular, same eviction as stats_engine */
    float    buf_x[STATS_WINDOW_SIZE];
    float    buf_y[STATS_WINDOW_SIZE];
    int      head;             /* next write index                          */
    int      n;                /* samples in window (capped at WINDOW_SIZE) */

    /* Computed correlation state */
    float    current_r;        /* Pearson r from most recent window         */
    float    ema_r;            /* EMA baseline of r                         */
    float    ema_r_var;        /* EMA variance of r                         */
    bool     baseline_seeded;  /* first valid r has seeded ema_r            */
    uint32_t samples_total;    /* total samples ever accepted               */
    bool     ready;            /* n >= CORRELATION_MIN_SAMPLES AND baseline_seeded */

    /* Alert state — independent from anomaly_engine */
    bool     alert_active;
    int      consecutive_anomalous;
    int      consecutive_normal;
} corr_pair_t;

/* ── module state ────────────────────────────────────────────────────── */

typedef struct {
    corr_pair_t      pairs[CORR_PAIR_COUNT];
    SemaphoreHandle_t mutex;
} tracker_state_t;

static tracker_state_t s_state;

/* ── Pearson r computation ───────────────────────────────────────────── */

/*
 * Compute Pearson r over the n samples currently in pair's circular buffers.
 * Returns 0.0f (and sets *valid = false) when either variable has near-zero
 * variance in the window — degenerate case, must not feed EMA.
 *
 * Two-pass:
 *   Pass 1: compute means of x and y over the window.
 *   Pass 2: accumulate numerator Σ(xi-x̄)(yi-ȳ), and
 *           denominator terms Σ(xi-x̄)² and Σ(yi-ȳ)².
 *
 * Called outside the mutex — reads buf_x and buf_y which are written only
 * from udp_rx_task (the same task that calls this function).
 */
static float pearson_r(const corr_pair_t *p, bool *valid)
{
    *valid = false;

    int n = p->n;
    if (n < 2) return 0.0f;

    /* Pass 1 — means */
    double sum_x = 0.0, sum_y = 0.0;
    for (int i = 0; i < n; i++) {
        sum_x += p->buf_x[i];
        sum_y += p->buf_y[i];
    }
    double mean_x = sum_x / n;
    double mean_y = sum_y / n;

    /* Pass 2 — covariance terms */
    double num  = 0.0;   /* Σ(xi - x̄)(yi - ȳ) */
    double ss_x = 0.0;   /* Σ(xi - x̄)²         */
    double ss_y = 0.0;   /* Σ(yi - ȳ)²          */

    for (int i = 0; i < n; i++) {
        double dx = p->buf_x[i] - mean_x;
        double dy = p->buf_y[i] - mean_y;
        num  += dx * dy;
        ss_x += dx * dx;
        ss_y += dy * dy;
    }

    /* Degenerate guard: near-zero variance in either variable */
    double denom = sqrt(ss_x * ss_y);
    if (denom < 1e-9) return 0.0f;

    *valid = true;
    float r = (float)(num / denom);

    /* Clamp to [-1, 1] — floating point can produce very slight overflows */
    if (r >  1.0f) r =  1.0f;
    if (r < -1.0f) r = -1.0f;
    return r;
}

/* ── per-pair sample ingestion ───────────────────────────────────────── */

static void pair_add_sample(corr_pair_t *p, corr_pair_id_t id, float x, float y)
{
    /* Circular buffer eviction — overwrite oldest when full */
    p->buf_x[p->head] = x;
    p->buf_y[p->head] = y;
    p->head = (p->head + 1) % STATS_WINDOW_SIZE;

    if (p->n < STATS_WINDOW_SIZE) p->n++;
    p->samples_total++;

    /* ready flag is also conditioned on baseline_seeded — see ema_r_update */
    if (p->n >= CORRELATION_MIN_SAMPLES && p->baseline_seeded && !p->ready) {
        p->ready = true;
        ESP_LOGI(TAG, "%s correlation ready (n=%d)", PAIR_NAME[id], p->n);
    }
}

/* ── EMA update for r baseline ───────────────────────────────────────── */

static void ema_r_update(corr_pair_t *p, float r)
{
    if (!p->baseline_seeded) {
        /* Seed directly from first valid r — avoids zero-start bias */
        p->ema_r           = r;
        p->ema_r_var       = 0.0f;
        p->baseline_seeded = true;
        return;
    }

    float diff    = r - p->ema_r;
    p->ema_r     += CORRELATION_ALPHA * diff;
    p->ema_r_var  = (1.0f - CORRELATION_ALPHA)
                  * (p->ema_r_var + CORRELATION_ALPHA * diff * diff);
}

/* ── alert check for one pair ────────────────────────────────────────── */

static void check_pair_alert(corr_pair_t *p, corr_pair_id_t id,
                              float r, float ema_r, float ema_r_var)
{
    /* Variance floor — suppress when r baseline is too stable to score */
    if (ema_r_var < CORRELATION_VAR_FLOOR) return;

    float stddev = sqrtf(ema_r_var);
    float z      = (r - ema_r) / stddev;
    bool  anomalous = (fabsf(z) > CORRELATION_ZSCORE_THRESH);

    if (anomalous) {
        p->consecutive_anomalous++;
        p->consecutive_normal = 0;

        if (!p->alert_active
            && p->consecutive_anomalous >= CORRELATION_CONSECUTIVE_THRESH) {
            p->alert_active = true;

            char msg[ALERT_MSG_LEN];
            snprintf(msg, sizeof(msg),
                     "%s correlation anomaly: r=%.3f ema_r=%.3f stddev=%.3f z=%.2f",
                     PAIR_NAME[id], r, ema_r, stddev, z);

            alert_manager_raise(ALERT_LEVEL_WARNING,
                                ALERT_TYPE_CORRELATION_ANOMALY,
                                msg, r, ema_r, stddev, z);
        }
    } else {
        p->consecutive_normal++;
        p->consecutive_anomalous = 0;

        if (p->alert_active
            && p->consecutive_normal >= CORRELATION_CONSECUTIVE_THRESH) {
            p->alert_active = false;
            ESP_LOGI(TAG, "%s correlation returned to normal", PAIR_NAME[id]);
        }
    }
}

/* ── public API ──────────────────────────────────────────────────────── */

void correlation_tracker_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG,
             "Initialised. Pairs: %d  min_samples: %d  alpha: %.3f  "
             "z_thresh: %.1f  var_floor: %.4f",
             CORR_PAIR_COUNT,
             CORRELATION_MIN_SAMPLES,
             (double)CORRELATION_ALPHA,
             (double)CORRELATION_ZSCORE_THRESH,
             (double)CORRELATION_VAR_FLOOR);
}

void correlation_tracker_update(const stats_snapshot_t *snap,
                                const behavior_profile_snapshot_t *bp,
                                bool stream_active)
{
    /* ── Layer 1: Phase 1 still learning ── */
    if (snap->learning) return;

    /*
     * Suppressed internally when Phase 1 learning is still active (Layer 1).
     * Layer 3 (per-pair buffer minimum) is enforced independently per pair
     * via pair_add_sample() and the ready flag — no global gate needed here.
     *
     * Extract per-heartbeat metric values from the stats snapshot.
     * stream_active comes directly from the heartbeat packet via the caller —
     * it is NOT derived from viewer_count. This is required to detect the
     * primary threat case: stream_active=1 with viewer_count=0.
     */
    float rssi        = snap->rssi.latest;
    float reconnect   = snap->reconnect_rate.latest;
    float hb_interval = snap->heartbeat_interval_ms.latest;
    float viewers     = snap->viewer_count.latest;
    float stream      = stream_active ? 1.0f : 0.0f;

    /* Skip HB interval pair if no valid delta exists yet (< 2 samples) */
    bool hb_valid = (snap->heartbeat_interval_ms.count >= 2);

    /*
     * Add samples and compute Pearson r outside the mutex.
     * buf_x and buf_y are written only from udp_rx_task — no concurrent writer.
     */
    bool r0_valid, r1_valid, r2_valid;
    float r0, r1, r2;

    /* Pair 0: RSSI ↔ reconnect_rate */
    pair_add_sample(&s_state.pairs[CORR_PAIR_RSSI_RECONNECT],
                    CORR_PAIR_RSSI_RECONNECT, rssi, reconnect);
    r0 = pearson_r(&s_state.pairs[CORR_PAIR_RSSI_RECONNECT], &r0_valid);

    /* Pair 1: RSSI ↔ heartbeat_interval */
    if (hb_valid) {
        pair_add_sample(&s_state.pairs[CORR_PAIR_RSSI_HB_INTERVAL],
                        CORR_PAIR_RSSI_HB_INTERVAL, rssi, hb_interval);
    }
    r1 = pearson_r(&s_state.pairs[CORR_PAIR_RSSI_HB_INTERVAL], &r1_valid);

    /* Pair 2: stream_active ↔ viewer_count */
    pair_add_sample(&s_state.pairs[CORR_PAIR_STREAM_VIEWERS],
                    CORR_PAIR_STREAM_VIEWERS, stream, viewers);
    r2 = pearson_r(&s_state.pairs[CORR_PAIR_STREAM_VIEWERS], &r2_valid);

    /*
     * Take mutex to write results and run alert checks.
     * Alert checks run inside the mutex so alert_active flags are
     * consistent with the EMA state they read.
     */
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    /* Pair 0 */
    if (r0_valid) {
        ema_r_update(&s_state.pairs[CORR_PAIR_RSSI_RECONNECT], r0);
        s_state.pairs[CORR_PAIR_RSSI_RECONNECT].current_r = r0;
        if (s_state.pairs[CORR_PAIR_RSSI_RECONNECT].ready) {
            check_pair_alert(&s_state.pairs[CORR_PAIR_RSSI_RECONNECT],
                             CORR_PAIR_RSSI_RECONNECT,
                             r0,
                             s_state.pairs[CORR_PAIR_RSSI_RECONNECT].ema_r,
                             s_state.pairs[CORR_PAIR_RSSI_RECONNECT].ema_r_var);
        }
    }

    /* Pair 1 */
    if (r1_valid) {
        ema_r_update(&s_state.pairs[CORR_PAIR_RSSI_HB_INTERVAL], r1);
        s_state.pairs[CORR_PAIR_RSSI_HB_INTERVAL].current_r = r1;
        if (s_state.pairs[CORR_PAIR_RSSI_HB_INTERVAL].ready) {
            check_pair_alert(&s_state.pairs[CORR_PAIR_RSSI_HB_INTERVAL],
                             CORR_PAIR_RSSI_HB_INTERVAL,
                             r1,
                             s_state.pairs[CORR_PAIR_RSSI_HB_INTERVAL].ema_r,
                             s_state.pairs[CORR_PAIR_RSSI_HB_INTERVAL].ema_r_var);
        }
    }

    /* Pair 2 */
    if (r2_valid) {
        ema_r_update(&s_state.pairs[CORR_PAIR_STREAM_VIEWERS], r2);
        s_state.pairs[CORR_PAIR_STREAM_VIEWERS].current_r = r2;
        if (s_state.pairs[CORR_PAIR_STREAM_VIEWERS].ready) {
            check_pair_alert(&s_state.pairs[CORR_PAIR_STREAM_VIEWERS],
                             CORR_PAIR_STREAM_VIEWERS,
                             r2,
                             s_state.pairs[CORR_PAIR_STREAM_VIEWERS].ema_r,
                             s_state.pairs[CORR_PAIR_STREAM_VIEWERS].ema_r_var);
        }
    }

    /*
     * Update per-pair ready flag inside the mutex so that snapshot readers
     * always see a consistent (ready, current_r, ema_r) tuple.
     * ready requires both n >= CORRELATION_MIN_SAMPLES and baseline_seeded.
     * baseline_seeded is set by ema_r_update() on the first valid r above,
     * so this check is valid immediately after the ema_r_update calls.
     */
    for (int i = 0; i < CORR_PAIR_COUNT; i++) {
        corr_pair_t *p = &s_state.pairs[i];
        if (!p->ready && p->n >= CORRELATION_MIN_SAMPLES && p->baseline_seeded) {
            p->ready = true;
            ESP_LOGI(TAG, "%s correlation ready (n=%d)", PAIR_NAME[i], p->n);
        }
    }

    xSemaphoreGive(s_state.mutex);
}

correlation_snapshot_t correlation_tracker_get_snapshot(void)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    correlation_snapshot_t out;
    memset(&out, 0, sizeof(out));
    bool all_ready = true;

    for (int i = 0; i < CORR_PAIR_COUNT; i++) {
        const corr_pair_t *p = &s_state.pairs[i];

        out.pairs[i].current_r    = p->current_r;
        out.pairs[i].ema_r        = p->ema_r;
        out.pairs[i].ema_r_stddev = (p->ema_r_var > 0.0f)
                                    ? sqrtf(p->ema_r_var) : 0.0f;
        out.pairs[i].alert_active = p->alert_active;
        out.pairs[i].ready        = p->ready;

        /* Compute z-score for snapshot — valid only when pair is ready */
        if (p->ready && p->ema_r_var >= CORRELATION_VAR_FLOOR) {
            out.pairs[i].zscore = (p->current_r - p->ema_r)
                                  / sqrtf(p->ema_r_var);
        } else {
            out.pairs[i].zscore = 0.0f;
        }

        if (!out.pairs[i].ready) all_ready = false;
    }

    out.ready = all_ready;

    xSemaphoreGive(s_state.mutex);
    return out;
}
