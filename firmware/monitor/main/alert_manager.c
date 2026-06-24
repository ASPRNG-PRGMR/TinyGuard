/*
 * alert_manager.c — TinyGuard Monitor
 *
 * Circular alert buffer with serial logging on every raise.
 * Dashboard (Milestone 5) will read from alert_manager_get_recent().
 */

#include "alert_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <inttypes.h>
#include <stdio.h>

static const char *TAG = "Alert";

static const char *level_str[] = { "INFO", "WARN", "CRIT" };

static const char *type_str[] = {
    "DEVICE_CONNECTED",
    "LEARNING_COMPLETE",
    "HEARTBEAT_MISSING",
    "DEVICE_OFFLINE",
    "RSSI_ANOMALY",
    "RECONNECT_ANOMALY",
    "STREAM_ANOMALY",
    "CORRELATION_ANOMALY",
};

static alert_t           s_buf[ALERT_HISTORY_SIZE];
static int               s_head  = 0;
static uint32_t          s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

void alert_manager_init(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    s_head  = 0;
    s_count = 0;
    s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Alert manager ready. History size: %d", ALERT_HISTORY_SIZE);
}

void alert_manager_raise(alert_level_t level, alert_type_t type,
                          const char *message,
                          float observed, float mean,
                          float stddev, float zscore)
{
    alert_t a = {
        .timestamp_ms = esp_log_timestamp(),
        .level        = level,
        .type         = type,
        .observed     = observed,
        .mean         = mean,
        .stddev       = stddev,
        .zscore       = zscore,
    };
    strncpy(a.message, message, ALERT_MSG_LEN - 1);
    a.message[ALERT_MSG_LEN - 1] = '\0';

    /* Log to serial immediately, before taking the mutex */
    switch (level) {
        case ALERT_LEVEL_INFO:
            ESP_LOGI(TAG, "[%s] %s: %s",
                     level_str[level], type_str[type], message);
            break;
        case ALERT_LEVEL_WARNING:
            ESP_LOGW(TAG, "[%s] %s: %s | obs=%.2f mean=%.2f stddev=%.2f z=%.2f",
                     level_str[level], type_str[type], message,
                     observed, mean, stddev, zscore);
            break;
        case ALERT_LEVEL_CRITICAL:
            ESP_LOGE(TAG, "[%s] %s: %s",
                     level_str[level], type_str[type], message);
            break;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_buf[s_head] = a;
    s_head = (s_head + 1) % ALERT_HISTORY_SIZE;
    s_count++;
    xSemaphoreGive(s_mutex);
}

void alert_raise_info(alert_type_t type, const char *message)
{
    alert_manager_raise(ALERT_LEVEL_INFO, type, message,
                        0.0f, 0.0f, 0.0f, 0.0f);
}

void alert_raise_critical(alert_type_t type, const char *message)
{
    alert_manager_raise(ALERT_LEVEL_CRITICAL, type, message,
                        0.0f, 0.0f, 0.0f, 0.0f);
}

uint32_t alert_manager_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t c = s_count;
    xSemaphoreGive(s_mutex);
    return c;
}

int alert_manager_get_recent(alert_t *out, int max_count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int filled = (s_count < ALERT_HISTORY_SIZE)
                 ? (int)s_count
                 : ALERT_HISTORY_SIZE;
    int n = (max_count < filled) ? max_count : filled;

    /*
     * Walk backwards from head to get most-recent first,
     * then reverse into out[] so index 0 = oldest of the n returned.
     */
    for (int i = 0; i < n; i++) {
        int idx = (s_head - 1 - i + ALERT_HISTORY_SIZE) % ALERT_HISTORY_SIZE;
        out[n - 1 - i] = s_buf[idx];
    }

    xSemaphoreGive(s_mutex);
    return n;
}
