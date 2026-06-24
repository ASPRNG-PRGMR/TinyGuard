/*
 * anomaly_engine.c — TinyGuard Monitor
 *
 * Z-score detection against rolling baselines.
 *
 * Structure:
 *   anomaly_engine_evaluate()  — called after every heartbeat; runs all
 *                                metric checks; no-op during learning phase
 *   watchdog_task()            — separate FreeRTOS task; polls for heartbeat
 *                                timeout every second
 *
 * Per-metric state:
 *   Each metric has a consecutive-hit counter. It increments when a sample
 *   is anomalous and resets to zero when the sample is normal. An alert
 *   fires only when the counter reaches ANOMALY_CONSECUTIVE_THRESH.
 *   This prevents a single transient spike from generating noise.
 *
 * Alert deduplication:
 *   Once a metric alert fires, a cooldown flag is set. The flag clears when
 *   the metric returns to normal for ANOMALY_CONSECUTIVE_THRESH consecutive
 *   samples. This prevents alert storms when a device is in a sustained
 *   anomalous state.
 */

#include "anomaly_engine.h"
#include "alert_manager.h"
#include "stats_engine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <inttypes.h>
#include <stdio.h>

static const char *TAG = "Anomaly";

/* ── per-metric tracking ─────────────────────────────────────────────── */

typedef struct {
    int  consecutive_anomalous;  /* increments each bad sample      */
    int  consecutive_normal;     /* increments each good sample      */
    bool alert_active;           /* true = alert already fired; suppress repeats */
} metric_state_t;

static metric_state_t s_rssi;
static metric_state_t s_hb_interval;
static metric_state_t s_reconnect;
static metric_state_t s_viewers;

/* ── heartbeat watchdog state ────────────────────────────────────────── */

static volatile uint32_t s_last_hb_ms    = 0;
static volatile bool     s_hb_timeout_active = false;
static SemaphoreHandle_t s_hb_mutex      = NULL;

/* ── z-score helper ──────────────────────────────────────────────────── */

static float zscore(float observed, float mean, float stddev)
{
    if (stddev < ANOMALY_MIN_STDDEV) return 0.0f;
    return (observed - mean) / stddev;
}

/* ── per-metric evaluation ───────────────────────────────────────────── */

/*
 * Generic check: given a z-score and the metric's tracking state,
 * decide whether to raise or clear an alert.
 *
 * Returns true if an alert was just raised this call.
 */
static bool check_metric(metric_state_t *ms, float z,
                          float observed, float mean, float stddev,
                          alert_type_t type, const char *label)
{
    bool anomalous = (fabsf(z) > ANOMALY_ZSCORE_THRESHOLD);

    if (anomalous) {
        ms->consecutive_anomalous++;
        ms->consecutive_normal = 0;

        if (!ms->alert_active
            && ms->consecutive_anomalous >= ANOMALY_CONSECUTIVE_THRESH) {
            ms->alert_active = true;

            char msg[ALERT_MSG_LEN];
            snprintf(msg, sizeof(msg),
                     "%s anomaly: obs=%.2f mean=%.2f stddev=%.2f z=%.2f",
                     label, observed, mean, stddev, z);

            alert_manager_raise(ALERT_LEVEL_WARNING, type, msg,
                                 observed, mean, stddev, z);
            return true;
        }
    } else {
        ms->consecutive_normal++;
        ms->consecutive_anomalous = 0;

        /* Clear cooldown after THRESH consecutive normal samples */
        if (ms->alert_active
            && ms->consecutive_normal >= ANOMALY_CONSECUTIVE_THRESH) {
            ms->alert_active = false;
            ESP_LOGI(TAG, "%s returned to normal", label);
        }
    }
    return false;
}

/* ── heartbeat watchdog task ─────────────────────────────────────────── */

static void watchdog_task(void *arg)
{
    /* Wait until first heartbeat before starting timeout checks */
    while (true) {
        xSemaphoreTake(s_hb_mutex, portMAX_DELAY);
        bool started = (s_last_hb_ms != 0);
        xSemaphoreGive(s_hb_mutex);
        if (started) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Watchdog active. Timeout threshold: %d ms",
             HEARTBEAT_TIMEOUT_MS);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        xSemaphoreTake(s_hb_mutex, portMAX_DELAY);
        uint32_t last = s_last_hb_ms;
        xSemaphoreGive(s_hb_mutex);

        uint32_t now     = esp_log_timestamp();
        uint32_t elapsed = now - last;

        if (elapsed >= HEARTBEAT_TIMEOUT_MS) {
            if (!s_hb_timeout_active) {
                s_hb_timeout_active = true;
                char msg[ALERT_MSG_LEN];
                snprintf(msg, sizeof(msg),
                         "No heartbeat for %" PRIu32 " ms (threshold %d ms)",
                         elapsed, HEARTBEAT_TIMEOUT_MS);
                alert_raise_critical(ALERT_TYPE_HEARTBEAT_MISSING, msg);
            }
        } else {
            if (s_hb_timeout_active) {
                s_hb_timeout_active = false;
                alert_raise_info(ALERT_TYPE_DEVICE_CONNECTED,
                                 "Heartbeat resumed — device back online");
            }
        }
    }
}

/* ── public API ──────────────────────────────────────────────────────── */

void anomaly_engine_init(void)
{
    s_rssi        = (metric_state_t){0};
    s_hb_interval = (metric_state_t){0};
    s_reconnect   = (metric_state_t){0};
    s_viewers     = (metric_state_t){0};

    s_last_hb_ms        = 0;
    s_hb_timeout_active = false;
    s_hb_mutex          = xSemaphoreCreateMutex();

    /* Start the heartbeat watchdog as a low-priority background task */
    xTaskCreate(watchdog_task, "hb_watchdog", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "Detection engine ready. Z threshold: %.1f  Consecutive: %d",
             ANOMALY_ZSCORE_THRESHOLD, ANOMALY_CONSECUTIVE_THRESH);
}

void anomaly_engine_heartbeat_received(void)
{
    xSemaphoreTake(s_hb_mutex, portMAX_DELAY);
    s_last_hb_ms = esp_log_timestamp();
    xSemaphoreGive(s_hb_mutex);
}

void anomaly_engine_evaluate(void)
{
    /* No-op during learning phase */
    if (stats_engine_is_learning()) return;

    stats_snapshot_t snap = stats_engine_get_snapshot();

    /* Need at least THRESH samples before stddev is meaningful */
    if (snap.samples_total < ANOMALY_CONSECUTIVE_THRESH) return;

    float z;

    /* ── RSSI ── */
    z = zscore(snap.rssi.latest, snap.rssi.mean, snap.rssi.stddev);
    check_metric(&s_rssi, z,
                 snap.rssi.latest, snap.rssi.mean, snap.rssi.stddev,
                 ALERT_TYPE_RSSI_ANOMALY, "RSSI");

    /* ── Heartbeat interval ── */
    if (snap.heartbeat_interval_ms.count >= 2) {
        z = zscore(snap.heartbeat_interval_ms.latest,
                   snap.heartbeat_interval_ms.mean,
                   snap.heartbeat_interval_ms.stddev);
        check_metric(&s_hb_interval, z,
                     snap.heartbeat_interval_ms.latest,
                     snap.heartbeat_interval_ms.mean,
                     snap.heartbeat_interval_ms.stddev,
                     ALERT_TYPE_HEARTBEAT_MISSING, "Heartbeat interval");
    }

    /* ── Reconnect rate ── */
    z = zscore(snap.reconnect_rate.latest,
               snap.reconnect_rate.mean,
               snap.reconnect_rate.stddev);
    check_metric(&s_reconnect, z,
                 snap.reconnect_rate.latest,
                 snap.reconnect_rate.mean,
                 snap.reconnect_rate.stddev,
                 ALERT_TYPE_RECONNECT_ANOMALY, "Reconnect rate");

    /* ── Stream viewers ── */
    z = zscore(snap.viewer_count.latest,
               snap.viewer_count.mean,
               snap.viewer_count.stddev);
    check_metric(&s_viewers, z,
                 snap.viewer_count.latest,
                 snap.viewer_count.mean,
                 snap.viewer_count.stddev,
                 ALERT_TYPE_STREAM_ANOMALY, "Stream viewers");
}

void anomaly_engine_heartbeat_timeout(void)
{
    /* External call path — watchdog_task handles this internally now */
    alert_raise_critical(ALERT_TYPE_DEVICE_OFFLINE, "Device offline");
}
