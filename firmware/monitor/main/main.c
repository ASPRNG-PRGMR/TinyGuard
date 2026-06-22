/*
 * TinyGuard Monitor — main.c
 * Milestone 3: WiFi + UDP receiver + device state + statistics engine
 */

#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "wifi_manager.h"
#include "device_state.h"
#include "udp_receiver.h"
#include "stats_engine.h"

static const char *TAG = "TinyGuard";

void app_main(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, "  TinyGuard Monitor — Milestone 3");
    ESP_LOGI(TAG, "  Static IP : 192.168.137.20");
    ESP_LOGI(TAG, "  UDP port  : 5000");
    ESP_LOGI(TAG, "═══════════════════════════════════════");

    wifi_manager_init();
    device_state_init();
    stats_engine_init();
    udp_receiver_start();

    ESP_LOGI(TAG, "Ready. Waiting for heartbeats ...");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));  /* print stats summary every 30s */

        stats_snapshot_t snap = stats_engine_get_snapshot();

        if (snap.samples_total == 0) {
            ESP_LOGW(TAG, "No heartbeats received yet.");
            continue;
        }

        if (snap.learning) {
            ESP_LOGI(TAG, "[LEARNING] %" PRIu32 " s remaining | samples: %" PRIu32,
                     snap.learning_remaining_s, snap.samples_total);
        } else {
            ESP_LOGI(TAG, "[ACTIVE] samples: %" PRIu32, snap.samples_total);
        }

        ESP_LOGI(TAG, "  RSSI      : mean=%.1f  stddev=%.1f  latest=%.0f dBm  (n=%d)",
                 snap.rssi.mean, snap.rssi.stddev,
                 snap.rssi.latest, snap.rssi.count);

        ESP_LOGI(TAG, "  HB intv   : mean=%.0f  stddev=%.0f  latest=%.0f ms  (n=%d)",
                 snap.heartbeat_interval_ms.mean,
                 snap.heartbeat_interval_ms.stddev,
                 snap.heartbeat_interval_ms.latest,
                 snap.heartbeat_interval_ms.count);

        ESP_LOGI(TAG, "  Recon/hr  : mean=%.2f  stddev=%.2f  latest=%.2f  (n=%d)",
                 snap.reconnect_rate.mean,
                 snap.reconnect_rate.stddev,
                 snap.reconnect_rate.latest,
                 snap.reconnect_rate.count);

        ESP_LOGI(TAG, "  Viewers   : mean=%.2f  stddev=%.2f  latest=%.0f  (n=%d)",
                 snap.viewer_count.mean,
                 snap.viewer_count.stddev,
                 snap.viewer_count.latest,
                 snap.viewer_count.count);
    }
}
