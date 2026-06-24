/*
 * behavior_profile.c — TinyGuard Monitor (Phase 2)
 *
 * Long-term behavioral profiling via seeded EMA with online variance.
 * See behavior_profile.h for full design rationale and algorithm.
 *
 * IMPLEMENTATION NOTES
 * --------------------
 *
 * Seeding path:
 *   behavior_profile_seed() is called exactly once. It copies the
 *   Phase 1 short-term means into ema_mean and sets seeded=true.
 *   Subsequent calls to behavior_profile_update() check seeded before
 *   ingesting samples, so no data is accumulated before the seed.
 *
 * ready flag:
 *   Set per-profile after n >= BEHAVIOR_PROFILE_MIN_SAMPLES post-seed.
 *   Logged once to serial so the operator knows when fingerprinting
 *   becomes active.
 *
 * ema_var guard:
 *   behavior_profile_zscore() suppresses output when ema_var < 0.25
 *   (effective stddev < 0.5). This mirrors Phase 1's ANOMALY_MIN_STDDEV
 *   and prevents division-by-near-zero on perfectly flat metrics.
 *
 * Heartbeat interval guard:
 *   The first heartbeat produces no interval sample (stats_engine skips
 *   it). We guard by checking snap->heartbeat_interval_ms.count >= 2
 *   before feeding that metric. Same guard is in anomaly_engine.c.
 */

#include "behavior_profile.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "BehaviorProfile";

/* ── internal state ──────────────────────────────────────────────────── */

typedef struct {
    long_term_stat_t rssi;
    long_term_stat_t heartbeat_interval_ms;
    long_term_stat_t reconnect_rate;
    long_term_stat_t viewer_count;

    bool seeded_done;        /* seed has been applied */
    SemaphoreHandle_t mutex;
} profile_state_t;

static profile_state_t s_state;

/* ── EMA helper ──────────────────────────────────────────────────────── */

/*
 * Apply one EMA update to a single metric profile.
 * Finch (2009) variance estimator: numerically stable, no buffer.
 *
 * Must only be called with mutex held and after seeded == true.
 */
static void ema_update(long_term_stat_t *p, float x, const char *name)
{
    float diff   = x - p->ema_mean;
    p->ema_mean += BEHAVIOR_PROFILE_ALPHA * diff;
    p->ema_var   = (1.0f - BEHAVIOR_PROFILE_ALPHA)
                 * (p->ema_var + BEHAVIOR_PROFILE_ALPHA * diff * diff);
    p->n++;

    if (!p->ready && p->n >= BEHAVIOR_PROFILE_MIN_SAMPLES) {
        p->ready = true;
        ESP_LOGI(TAG, "%s long-term profile ready: "
                 "ema_mean=%.2f  ema_stddev=%.2f  (n=%" PRIu32 ")",
                 name, p->ema_mean, sqrtf(p->ema_var), p->n);
    }
}

/* ── public API ──────────────────────────────────────────────────────── */

void behavior_profile_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG,
             "Initialised. alpha=%.3f  ready_after=%d samples (~%d min post-learning)",
             BEHAVIOR_PROFILE_ALPHA,
             BEHAVIOR_PROFILE_MIN_SAMPLES,
             (BEHAVIOR_PROFILE_MIN_SAMPLES * 10) / 60);
}

void behavior_profile_seed(const stats_snapshot_t *snap)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    if (s_state.seeded_done) {
        /* Guard: only seed once. If called again (shouldn't happen),
         * silently ignore to avoid corrupting an already-converging profile. */
        xSemaphoreGive(s_state.mutex);
        return;
    }

    /*
     * Seed ema_mean from Phase 1 short-term window means.
     * ema_var intentionally left at 0.0f — it builds from actual spread.
     * n and ready stay at 0/false — the profile starts counting from here.
     */
    s_state.rssi.ema_mean                  = snap->rssi.mean;
    s_state.heartbeat_interval_ms.ema_mean = snap->heartbeat_interval_ms.mean;
    s_state.reconnect_rate.ema_mean        = snap->reconnect_rate.mean;
    s_state.viewer_count.ema_mean          = snap->viewer_count.mean;

    s_state.rssi.seeded                  = true;
    s_state.heartbeat_interval_ms.seeded = true;
    s_state.reconnect_rate.seeded        = true;
    s_state.viewer_count.seeded          = true;

    s_state.seeded_done = true;

    ESP_LOGI(TAG, "Profiles seeded from Phase 1 baseline:");
    ESP_LOGI(TAG, "  RSSI             : %.1f dBm", snap->rssi.mean);
    ESP_LOGI(TAG, "  HB interval      : %.0f ms",  snap->heartbeat_interval_ms.mean);
    ESP_LOGI(TAG, "  Reconnect rate   : %.2f /hr", snap->reconnect_rate.mean);
    ESP_LOGI(TAG, "  Viewer count     : %.2f",     snap->viewer_count.mean);
    ESP_LOGI(TAG, "  Ready after: %d more samples (~%d min)",
             BEHAVIOR_PROFILE_MIN_SAMPLES,
             (BEHAVIOR_PROFILE_MIN_SAMPLES * 10) / 60);

    xSemaphoreGive(s_state.mutex);
}

void behavior_profile_update(const stats_snapshot_t *snap)
{
    /* Gate 1: Phase 1 still learning — do not build long-term profile yet. */
    if (snap->learning) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    /* Gate 2: Not yet seeded — seed must arrive before updates are useful. */
    if (!s_state.seeded_done) {
        xSemaphoreGive(s_state.mutex);
        return;
    }

    ema_update(&s_state.rssi,          snap->rssi.latest,          "RSSI");
    ema_update(&s_state.reconnect_rate, snap->reconnect_rate.latest, "ReconnectRate");
    ema_update(&s_state.viewer_count,   snap->viewer_count.latest,   "ViewerCount");

    /* Heartbeat interval: undefined until 2nd sample in stats_engine */
    if (snap->heartbeat_interval_ms.count >= 2) {
        ema_update(&s_state.heartbeat_interval_ms,
                   snap->heartbeat_interval_ms.latest,
                   "HBInterval");
    }

    xSemaphoreGive(s_state.mutex);
}

behavior_profile_snapshot_t behavior_profile_get_snapshot(void)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    behavior_profile_snapshot_t out;
    out.rssi                  = s_state.rssi;
    out.heartbeat_interval_ms = s_state.heartbeat_interval_ms;
    out.reconnect_rate        = s_state.reconnect_rate;
    out.viewer_count          = s_state.viewer_count;

    out.all_ready = s_state.rssi.ready
                 && s_state.heartbeat_interval_ms.ready
                 && s_state.reconnect_rate.ready
                 && s_state.viewer_count.ready;

    xSemaphoreGive(s_state.mutex);
    return out;
}

float behavior_profile_zscore(const long_term_stat_t *profile, float current)
{
    if (!profile->ready)           return 0.0f;

    /*
     * Suppress when variance is near zero.
     * ema_var < 0.25 → effective stddev < 0.5.
     * Matches ANOMALY_MIN_STDDEV from Phase 1; prevents exploding z-scores
     * when a metric has been perfectly stable in the long-term profile.
     */
    if (profile->ema_var < 0.25f)  return 0.0f;

    return (current - profile->ema_mean) / sqrtf(profile->ema_var);
}
