/*
 * TinyGuard Monitor — main.c
 * Milestone 7: WiFi + UDP + device state + stats + detection + dashboard
 *              + long-term behavioral profiling (behavior_profile)
 */

#include <stdio.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "wifi_manager.h"
#include "device_state.h"
#include "udp_receiver.h"
#include "stats_engine.h"
#include "anomaly_engine.h"
#include "alert_manager.h"
#include "dashboard_server.h"
#include "behavior_profile.h"

static const char *TAG = "TinyGuard";

void app_main(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, "  TinyGuard Monitor — Milestone 7");
    ESP_LOGI(TAG, "  Static IP : 192.168.137.20");
    ESP_LOGI(TAG, "  UDP port  : 5000");
    ESP_LOGI(TAG, "  Dashboard : http://192.168.137.20/");
    ESP_LOGI(TAG, "═══════════════════════════════════════");

    wifi_manager_init();
    device_state_init();
    stats_engine_init();
    alert_manager_init();
    anomaly_engine_init();
    behavior_profile_init();
    dashboard_server_start();
    udp_receiver_start();

    alert_raise_info(ALERT_TYPE_DEVICE_CONNECTED, "Monitor started");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        stats_snapshot_t snap = stats_engine_get_snapshot();

        if (snap.samples_total == 0) {
            ESP_LOGW(TAG, "No heartbeats received yet.");
            continue;
        }

        if (snap.learning) {
            ESP_LOGI(TAG, "[LEARNING] %" PRIu32 " s remaining | samples: %" PRIu32,
                     snap.learning_remaining_s, snap.samples_total);
        } else {
            ESP_LOGI(TAG, "[ACTIVE] samples: %" PRIu32 " | alerts: %" PRIu32,
                     snap.samples_total, alert_manager_count());
        }

        ESP_LOGI(TAG, "  RSSI     : mean=%.1f  stddev=%.1f  latest=%.0f dBm",
                 snap.rssi.mean, snap.rssi.stddev, snap.rssi.latest);
        ESP_LOGI(TAG, "  HB intv  : mean=%.0f  stddev=%.0f  latest=%.0f ms",
                 snap.heartbeat_interval_ms.mean,
                 snap.heartbeat_interval_ms.stddev,
                 snap.heartbeat_interval_ms.latest);
        ESP_LOGI(TAG, "  Recon/hr : mean=%.2f  stddev=%.2f  latest=%.2f",
                 snap.reconnect_rate.mean,
                 snap.reconnect_rate.stddev,
                 snap.reconnect_rate.latest);
        ESP_LOGI(TAG, "  Viewers  : mean=%.2f  stddev=%.2f  latest=%.0f",
                 snap.viewer_count.mean,
                 snap.viewer_count.stddev,
                 snap.viewer_count.latest);

        /* Phase 2: log long-term profile state every 30 s */
        behavior_profile_snapshot_t bp = behavior_profile_get_snapshot();
        if (bp.rssi.seeded) {
            ESP_LOGI(TAG, "  [LT-PROFILE] ready=%s",
                     bp.all_ready ? "ALL" : "partial");
            ESP_LOGI(TAG, "    RSSI     : ema=%.1f  ema_stddev=%.2f  n=%" PRIu32,
                     bp.rssi.ema_mean, sqrtf(bp.rssi.ema_var), bp.rssi.n);
            ESP_LOGI(TAG, "    HB intv  : ema=%.0f  ema_stddev=%.2f  n=%" PRIu32,
                     bp.heartbeat_interval_ms.ema_mean,
                     sqrtf(bp.heartbeat_interval_ms.ema_var),
                     bp.heartbeat_interval_ms.n);
        }
    }
}
