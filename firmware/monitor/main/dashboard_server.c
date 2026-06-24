/*
 * dashboard_server.c — TinyGuard Monitor
 *
 * HTTP server on port 80. Two routes:
 *
 *   GET /        — HTML dashboard, auto-refreshes every 2 seconds
 *   GET /status  — JSON snapshot for curl / future tooling
 *
 * Implementation notes:
 *   - Uses ESP-IDF's esp_http_server (httpd), not a raw socket.
 *     httpd handles connection lifecycle, keeps the task footprint low,
 *     and is already in the ESP-IDF component tree — no extra deps.
 *   - HTML is built into a static char buffer per request (stack-allocated).
 *     No filesystem, no SPIFFS, no external assets. The page is ~3 KB
 *     rendered, well within a single TCP send.
 *   - All data comes from device_state_get(), stats_engine_get_snapshot(),
 *     and alert_manager_get_recent() — all mutex-safe reads.
 *   - The JSON endpoint returns the same data in machine-readable form.
 *     Field names match the heartbeat packet schema where applicable.
 */

#include "dashboard_server.h"
#include "device_state.h"
#include "stats_engine.h"
#include "alert_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "Dashboard";

/* ── helpers ─────────────────────────────────────────────────────────── */

static const char *alert_level_label(alert_level_t l)
{
    switch (l) {
        case ALERT_LEVEL_INFO:     return "INFO";
        case ALERT_LEVEL_WARNING:  return "WARN";
        case ALERT_LEVEL_CRITICAL: return "CRIT";
        default:                   return "????";
    }
}

static const char *alert_level_color(alert_level_t l)
{
    switch (l) {
        case ALERT_LEVEL_INFO:     return "#2196F3";
        case ALERT_LEVEL_WARNING:  return "#FF9800";
        case ALERT_LEVEL_CRITICAL: return "#F44336";
        default:                   return "#9E9E9E";
    }
}

/* ── HTML handler ────────────────────────────────────────────────────── */

static esp_err_t handle_root(httpd_req_t *req)
{
    device_state_t   dev  = device_state_get();
    stats_snapshot_t snap = stats_engine_get_snapshot();

    alert_t recent[8];
    int     alert_count = alert_manager_get_recent(recent, 8);

    /* Determine overall status */
    const char *status_text  = "OFFLINE";
    const char *status_color = "#F44336";

    if (dev.valid) {
        uint32_t age_ms = esp_log_timestamp() - dev.last_rx_tick;
        if (age_ms < 30000) {
            if (snap.learning) {
                status_text  = "LEARNING";
                status_color = "#2196F3";
            } else {
                status_text  = "MONITORING";
                status_color = "#4CAF50";
            }
        } else {
            status_text  = "TIMEOUT";
            status_color = "#F44336";
        }
    }

    /* Build HTML into a stack buffer.
     * Sections written separately to keep each snprintf manageable. */
    static char html[4096];
    int pos = 0;
    int rem = sizeof(html);

#define APPEND(...) do { \
    int _n = snprintf(html + pos, rem, __VA_ARGS__); \
    if (_n > 0) { pos += _n; rem -= _n; } \
} while (0)

    /* ── Head ── */
    APPEND(
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='2'>"
        "<title>TinyGuard</title>"
        "<style>"
        "body{font-family:monospace;background:#121212;color:#e0e0e0;margin:0;padding:16px}"
        "h1{color:#fff;margin:0 0 4px}h2{color:#bbb;font-size:.9em;margin:0 0 16px}"
        ".card{background:#1e1e1e;border-radius:8px;padding:16px;margin-bottom:12px}"
        ".row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #2a2a2a}"
        ".row:last-child{border-bottom:none}"
        ".label{color:#888}.value{color:#fff;font-weight:bold}"
        ".badge{display:inline-block;padding:4px 10px;border-radius:4px;font-weight:bold;color:#fff}"
        ".alert-row{padding:6px 0;border-bottom:1px solid #2a2a2a;font-size:.85em}"
        ".alert-row:last-child{border-bottom:none}"
        "</style></head><body>"
    );

    /* ── Header ── */
    APPEND(
        "<h1>TinyGuard</h1>"
        "<h2>IoT Camera Behavior Monitor</h2>"
        "<div class='card'>"
        "<div class='row'><span class='label'>Status</span>"
        "<span class='badge' style='background:%s'>%s</span></div>",
        status_color, status_text
    );

    if (dev.valid) {
        uint32_t age_ms = esp_log_timestamp() - dev.last_rx_tick;
        APPEND(
            "<div class='row'><span class='label'>Last heartbeat</span>"
            "<span class='value'>%" PRIu32 " ms ago</span></div>"
            "<div class='row'><span class='label'>Camera uptime</span>"
            "<span class='value'>%" PRIu32 " s</span></div>"
            "<div class='row'><span class='label'>RSSI</span>"
            "<span class='value'>%d dBm</span></div>"
            "<div class='row'><span class='label'>Stream</span>"
            "<span class='value'>%s</span></div>"
            "<div class='row'><span class='label'>Viewers</span>"
            "<span class='value'>%d</span></div>"
            "<div class='row'><span class='label'>Reconnects</span>"
            "<span class='value'>%d</span></div>"
            "<div class='row'><span class='label'>Packets received</span>"
            "<span class='value'>%" PRIu32 "</span></div>",
            age_ms,
            dev.timestamp,
            dev.rssi,
            dev.stream_active ? "ACTIVE" : "idle",
            dev.viewer_count,
            dev.reconnects,
            dev.packet_count
        );
    } else {
        APPEND("<div class='row'><span class='label'>No data yet</span></div>");
    }
    APPEND("</div>"); /* end status card */

    /* ── Statistics card ── */
    APPEND("<div class='card'>"
           "<div class='row'><span class='label' style='color:#fff;font-weight:bold'>"
           "Statistics</span>"
           "<span class='value'>%s | %" PRIu32 " samples</span></div>",
           snap.learning ? "LEARNING" : "ACTIVE",
           snap.samples_total);

    if (snap.learning && snap.learning_remaining_s > 0) {
        APPEND(
            "<div class='row'><span class='label'>Learning remaining</span>"
            "<span class='value'>%" PRIu32 " s</span></div>",
            snap.learning_remaining_s
        );
    }

    APPEND(
        "<div class='row'><span class='label'>RSSI mean / stddev</span>"
        "<span class='value'>%.1f / %.1f dBm</span></div>"
        "<div class='row'><span class='label'>HB interval mean / stddev</span>"
        "<span class='value'>%.0f / %.0f ms</span></div>"
        "<div class='row'><span class='label'>Reconnects/hr mean / stddev</span>"
        "<span class='value'>%.2f / %.2f</span></div>"
        "<div class='row'><span class='label'>Viewers mean / stddev</span>"
        "<span class='value'>%.2f / %.2f</span></div>"
        "</div>",
        snap.rssi.mean, snap.rssi.stddev,
        snap.heartbeat_interval_ms.mean, snap.heartbeat_interval_ms.stddev,
        snap.reconnect_rate.mean, snap.reconnect_rate.stddev,
        snap.viewer_count.mean, snap.viewer_count.stddev
    );

    /* ── Alert history card ── */
    APPEND(
        "<div class='card'>"
        "<div class='row'><span class='label' style='color:#fff;font-weight:bold'>"
        "Alert History</span>"
        "<span class='value'>%" PRIu32 " total</span></div>",
        alert_manager_count()
    );

    if (alert_count == 0) {
        APPEND("<div class='alert-row' style='color:#888'>No alerts yet</div>");
    } else {
        /* Show most recent first */
        for (int i = alert_count - 1; i >= 0; i--) {
            alert_t *a = &recent[i];
            APPEND(
                "<div class='alert-row'>"
                "<span class='badge' style='background:%s;font-size:.75em'>%s</span> "
                "<span style='color:#aaa'>+%" PRIu32 "ms</span> %s"
                "</div>",
                alert_level_color(a->level),
                alert_level_label(a->level),
                a->timestamp_ms,
                a->message
            );
        }
    }
    APPEND("</div>"); /* end alert card */

    /* ── Footer ── */
    APPEND(
        "<p style='color:#555;font-size:.75em;text-align:center'>"
        "TinyGuard &mdash; refreshes every 2s &mdash; "
        "<a href='/status' style='color:#555'>JSON</a></p>"
        "</body></html>"
    );

#undef APPEND

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

/* ── JSON handler ────────────────────────────────────────────────────── */

static esp_err_t handle_status(httpd_req_t *req)
{
    device_state_t   dev  = device_state_get();
    stats_snapshot_t snap = stats_engine_get_snapshot();

    alert_t recent[4];
    int     alert_count = alert_manager_get_recent(recent, 4);

    static char json[2048];
    int pos = 0;
    int rem = sizeof(json);

#define JAPPEND(...) do { \
    int _n = snprintf(json + pos, rem, __VA_ARGS__); \
    if (_n > 0) { pos += _n; rem -= _n; } \
} while (0)

    JAPPEND("{");
    JAPPEND("\"valid\":%s,", dev.valid ? "true" : "false");
    JAPPEND("\"rssi\":%d,", dev.rssi);
    JAPPEND("\"uptime_ms\":%" PRIu32 ",", dev.uptime_ms);
    JAPPEND("\"stream_active\":%s,", dev.stream_active ? "true" : "false");
    JAPPEND("\"viewer_count\":%d,", dev.viewer_count);
    JAPPEND("\"reconnects\":%d,", dev.reconnects);
    JAPPEND("\"packet_count\":%" PRIu32 ",", dev.packet_count);
    JAPPEND("\"learning\":%s,", snap.learning ? "true" : "false");
    JAPPEND("\"samples\":%" PRIu32 ",", snap.samples_total);
    JAPPEND("\"rssi_mean\":%.2f,", snap.rssi.mean);
    JAPPEND("\"rssi_stddev\":%.2f,", snap.rssi.stddev);
    JAPPEND("\"hb_interval_mean\":%.0f,", snap.heartbeat_interval_ms.mean);
    JAPPEND("\"hb_interval_stddev\":%.0f,", snap.heartbeat_interval_ms.stddev);
    JAPPEND("\"reconnect_rate_mean\":%.2f,", snap.reconnect_rate.mean);
    JAPPEND("\"viewer_mean\":%.2f,", snap.viewer_count.mean);
    JAPPEND("\"alert_count\":%" PRIu32 ",", alert_manager_count());
    JAPPEND("\"alerts\":[");
    for (int i = 0; i < alert_count; i++) {
        JAPPEND("{\"level\":%d,\"msg\":\"%s\",\"ts\":%" PRIu32 "}%s",
                (int)recent[i].level,
                recent[i].message,
                recent[i].timestamp_ms,
                (i < alert_count - 1) ? "," : "");
    }
    JAPPEND("]}");

#undef JAPPEND

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* ── start ───────────────────────────────────────────────────────────── */

void dashboard_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_open_sockets = 4;
    config.stack_size       = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = handle_root,
    };
    httpd_uri_t status_uri = {
        .uri      = "/status",
        .method   = HTTP_GET,
        .handler  = handle_status,
    };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &status_uri);

    ESP_LOGI(TAG, "Dashboard running at http://192.168.137.20/");
    ESP_LOGI(TAG, "JSON status at     http://192.168.137.20/status");
}
