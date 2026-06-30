/*
 * stats_engine.c — TinyGuard Monitor
 *
 * Rolling-window statistics using Welford's online algorithm.
 *
 * Why Welford instead of naive sum/count:
 *   The naive approach (mean = sum/n) accumulates floating-point error
 *   as n grows and is sensitive to catastrophic cancellation when values
 *   are close to the mean. Welford's method updates mean and variance
 *   incrementally with a single pass and remains numerically stable.
 *
 * Rolling window behaviour:
 *   When the buffer is full, the oldest sample is evicted. Welford's
 *   algorithm doesn't natively support sample removal, so on eviction
 *   we recompute mean/M2 from scratch over the window. This costs
 *   O(WINDOW_SIZE) on eviction but is cheap at our sample rates
 *   (one heartbeat per 10s = one recompute per 10s at steady state).
 *
 * Reconnect rate:
 *   The camera reports a cumulative reconnect counter. We derive
 *   reconnects-per-hour by tracking how many reconnects occurred in
 *   the rolling window and scaling to a one-hour equivalent.
 *   Formula: rate = (reconnects_in_window / window_duration_s) * 3600
 *
 * Thread safety:
 *   All state is protected by a single mutex. Stats update and snapshot
 *   reads are the only operations; both are short critical sections.
 */

#include "stats_engine.h"
#include "alert_manager.h"        // ← add this
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "Stats";

/* ── internal state ──────────────────────────────────────────────────── */

typedef struct {
    rolling_metric_t rssi;
    rolling_metric_t heartbeat_interval_ms;
    rolling_metric_t reconnect_rate;
    rolling_metric_t viewer_count;

    /* Heartbeat interval tracking */
    uint32_t last_rx_ms;        /* timestamp of previous heartbeat */
    bool     first_sample;      /* interval undefined until 2nd sample */

    /* Reconnect rate tracking */
    int      last_reconnect_count;  /* previous cumulative value from camera */
    uint32_t reconnect_window_start_ms;

    /* Learning phase */
    uint32_t learning_end_ms;   /* esp_log_timestamp() when learning ends */
    bool     learning_done;

    /* Total samples */
    uint32_t samples_total;

    SemaphoreHandle_t mutex;
} stats_state_t;

static stats_state_t s_state;

/* ── Welford helpers ─────────────────────────────────────────────────── */

static void metric_init(rolling_metric_t *m)
{
    memset(m, 0, sizeof(*m));
}

/*
 * Add a new sample. If the window is full, evict the oldest value and
 * recompute Welford accumulators from the remaining window contents.
 */
static void metric_add(rolling_metric_t *m, float value)
{
    bool full = (m->count == STATS_WINDOW_SIZE);

    if (full) {
        /* Overwrite oldest sample */
        m->buf[m->head] = value;
        m->head = (m->head + 1) % STATS_WINDOW_SIZE;

        /* Recompute Welford from scratch over the window */
        double mean = 0.0;
        double M2   = 0.0;
        for (int i = 0; i < STATS_WINDOW_SIZE; i++) {
            double delta = m->buf[i] - mean;
            mean += delta / (i + 1);
            M2   += delta * (m->buf[i] - mean);
        }
        m->mean = mean;
        m->M2   = M2;
    } else {
        /* Window not yet full — Welford incremental update */
        m->buf[m->head] = value;
        m->head = (m->head + 1) % STATS_WINDOW_SIZE;
        m->count++;

        double delta  = value - m->mean;
        m->mean      += delta / m->count;
        double delta2 = value - m->mean;
        m->M2        += delta * delta2;
    }

    m->n++;
}

static float metric_mean(const rolling_metric_t *m)
{
    if (m->count == 0) return 0.0f;
    return (float)m->mean;
}

static float metric_stddev(const rolling_metric_t *m)
{
    int n = (m->count < STATS_WINDOW_SIZE) ? m->count : STATS_WINDOW_SIZE;
    if (n < 2) return 0.0f;
    return (float)sqrtf((float)(m->M2 / (n - 1)));
}

static float metric_latest(const rolling_metric_t *m)
{
    if (m->count == 0) return 0.0f;
    /* head points to NEXT write slot, so latest is one before it */
    int latest_idx = (m->head - 1 + STATS_WINDOW_SIZE) % STATS_WINDOW_SIZE;
    return m->buf[latest_idx];
}

static metric_stats_t metric_snapshot(const rolling_metric_t *m)
{
    metric_stats_t s;
    s.mean   = metric_mean(m);
    s.stddev = metric_stddev(m);
    s.latest = metric_latest(m);
    s.count  = (m->count < STATS_WINDOW_SIZE) ? m->count : STATS_WINDOW_SIZE;
    return s;
}

/* ── reconnect rate ──────────────────────────────────────────────────── */

/*
 * Compute reconnects-per-hour from the change in cumulative counter
 * over the elapsed window time. Returns 0 on first call (no delta yet).
 */
static float compute_reconnect_rate(uint32_t now_ms, int current_count)
{
    if (s_state.last_reconnect_count < 0) {
        /* First sample — initialise baseline */
        s_state.last_reconnect_count   = current_count;
        s_state.reconnect_window_start_ms = now_ms;
        return 0.0f;
    }

    int delta_reconnects = current_count - s_state.last_reconnect_count;
    if (delta_reconnects < 0) delta_reconnects = 0; /* guard against reboot */

    uint32_t elapsed_ms = now_ms - s_state.reconnect_window_start_ms;
    if (elapsed_ms < 1000) return 0.0f; /* avoid divide-by-near-zero */

    float elapsed_s = elapsed_ms / 1000.0f;
    float rate_per_hour = ((float)delta_reconnects / elapsed_s) * 3600.0f;

    /* Slide window: update baseline to current values */
    s_state.last_reconnect_count      = current_count;
    s_state.reconnect_window_start_ms = now_ms;

    return rate_per_hour;
}

/* ── public API ──────────────────────────────────────────────────────── */

void stats_engine_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    metric_init(&s_state.rssi);
    metric_init(&s_state.heartbeat_interval_ms);
    metric_init(&s_state.reconnect_rate);
    metric_init(&s_state.viewer_count);

    s_state.first_sample          = true;
    s_state.last_reconnect_count  = -1;  /* sentinel: not yet seen */
    s_state.learning_done         = false;
    s_state.learning_end_ms       = 0;   /* set on first sample */
    s_state.samples_total         = 0;

    s_state.mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Initialised. Learning phase: %d s", STATS_LEARNING_DURATION_S);
}

void stats_engine_update(const stats_sample_t *in)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    uint32_t now_ms = in->heartbeat_rx_ms;

    /* Start learning timer on first sample */
    if (s_state.samples_total == 0) {
        s_state.learning_end_ms = now_ms + (STATS_LEARNING_DURATION_S * 1000);
        ESP_LOGI(TAG, "First sample received. Learning until %" PRIu32 " ms",
                 s_state.learning_end_ms);
    }
    
    char msg[ALERT_MSG_LEN];
    snprintf(msg, sizeof(msg),
         "Learning complete: RSSI baseline mean=%.1f dBm, %" PRIu32 " samples",
	 metric_mean(&s_state.rssi), s_state.samples_total);
    alert_raise_info(ALERT_TYPE_LEARNING_COMPLETE, msg);
    
    /* Check if learning phase just ended */
    if (!s_state.learning_done && now_ms >= s_state.learning_end_ms
        && s_state.samples_total > 0) {
        s_state.learning_done = true;
        ESP_LOGI(TAG, "Learning complete after %" PRIu32 " samples. Detection enabled.",
                 s_state.samples_total);
        ESP_LOGI(TAG, "  RSSI baseline    : mean=%.1f stddev=%.1f",
                 metric_mean(&s_state.rssi),
                 metric_stddev(&s_state.rssi));
        ESP_LOGI(TAG, "  HB interval base : mean=%.0f ms stddev=%.0f ms",
                 metric_mean(&s_state.heartbeat_interval_ms),
                 metric_stddev(&s_state.heartbeat_interval_ms));

        char msg[ALERT_MSG_LEN];
        snprintf(msg, sizeof(msg),
                 "Learning complete: RSSI baseline mean=%.1f dBm, %" PRIu32 " samples",
                 metric_mean(&s_state.rssi), s_state.samples_total);
        alert_raise_info(ALERT_TYPE_LEARNING_COMPLETE, msg);
    }

    /* ── RSSI ── */
    metric_add(&s_state.rssi, (float)in->rssi);

    /* ── Heartbeat interval ── */
    if (!s_state.first_sample) {
        uint32_t interval = now_ms - s_state.last_rx_ms;
        /* Sanity-clamp: ignore obviously wrong values (reboot, wrap) */
        if (interval > 0 && interval < 120000) {
            metric_add(&s_state.heartbeat_interval_ms, (float)interval);
        }
    } else {
        s_state.first_sample = false;
    }
    s_state.last_rx_ms = now_ms;

    /* ── Reconnect rate ── */
    float rate = compute_reconnect_rate(now_ms, in->reconnects);
    if (s_state.samples_total > 0) {  /* skip first sample (rate=0 by definition) */
        metric_add(&s_state.reconnect_rate, rate);
    }

    /* ── Viewer count ── */
    metric_add(&s_state.viewer_count, (float)in->viewer_count);

    s_state.samples_total++;

    xSemaphoreGive(s_state.mutex);
}

stats_snapshot_t stats_engine_get_snapshot(void)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    stats_snapshot_t snap;
    snap.rssi                  = metric_snapshot(&s_state.rssi);
    snap.heartbeat_interval_ms = metric_snapshot(&s_state.heartbeat_interval_ms);
    snap.reconnect_rate        = metric_snapshot(&s_state.reconnect_rate);
    snap.viewer_count          = metric_snapshot(&s_state.viewer_count);
    snap.learning              = !s_state.learning_done;
    snap.samples_total         = s_state.samples_total;

    if (!s_state.learning_done && s_state.learning_end_ms > 0) {
        uint32_t now = s_state.last_rx_ms;
        snap.learning_remaining_s = (s_state.learning_end_ms > now)
            ? (s_state.learning_end_ms - now) / 1000
            : 0;
    } else {
        snap.learning_remaining_s = 0;
    }

    xSemaphoreGive(s_state.mutex);
    return snap;
}

bool stats_engine_is_learning(void)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    bool learning = !s_state.learning_done;
    xSemaphoreGive(s_state.mutex);
    return learning;
}
