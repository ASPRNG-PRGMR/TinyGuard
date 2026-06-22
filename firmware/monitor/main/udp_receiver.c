/*
 * udp_receiver.c — TinyGuard Monitor
 *
 * Binds UDP socket on port 5000.
 * On each received packet:
 *   1. Null-terminates the buffer
 *   2. Parses JSON fields manually (no heap-allocating JSON lib needed)
 *   3. Updates device_state
 *   4. Prints a structured summary to serial
 *   5. Blinks the onboard LED once (GPIO2 on most DevKit V1 boards)
 *      — this is your visual confirmation when you have only one UART open
 *
 * JSON format expected (from heartbeat_service.cpp on camera):
 *   {"device":"esp32cam","uptime":10004,"rssi":-42,"stream_active":0,
 *    "viewer_count":0,"reconnects":0,"timestamp":10}
 *
 * Parsing strategy: strstr() + sscanf() per field.
 * Robust enough for a fixed-schema packet from our own firmware.
 * No dynamic allocation. No third-party JSON library required.
 */

#include "udp_receiver.h"
#include "device_state.h"
#include "stats_engine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "arpa/inet.h"

/* ── configuration ───────────────────────────────────────────────────── */
#define UDP_PORT        5000
#define RX_BUF_SIZE     512

/*
 * GPIO2 is the built-in blue LED on most ESP32 DevKit V1 boards.
 * It is active-HIGH. Change if your board differs.
 */
#define LED_GPIO        GPIO_NUM_2
#define LED_ACTIVE_HIGH 1
/* ──────────────────────────────────────────────────────────────────────── */

static const char *TAG = "UDP-RX";

/* ── LED helpers ─────────────────────────────────────────────────────── */

static void led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(LED_GPIO, !LED_ACTIVE_HIGH); /* off */
}

static void led_blink_once(void)
{
    gpio_set_level(LED_GPIO, LED_ACTIVE_HIGH);
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_level(LED_GPIO, !LED_ACTIVE_HIGH);
}

/* ── JSON field extraction ───────────────────────────────────────────── */
/*
 * Finds "key": and reads the integer value that follows.
 * Returns 0 if the key is not found (safe default for our fields).
 */
static long parse_int_field(const char *json, const char *key)
{
    /* Build search pattern  "key": */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *p = strstr(json, pattern);
    if (!p) return 0;

    p += strlen(pattern);

    /* Skip whitespace */
    while (*p == ' ' || *p == '\t') p++;

    return strtol(p, NULL, 10);
}

/* ── receiver task ───────────────────────────────────────────────────── */

static void udp_rx_task(void *arg)
{
    led_init();

    /* Create UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    /* Bind to all interfaces on UDP_PORT */
    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(UDP_PORT),
    };

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening on UDP port %d ...", UDP_PORT);

    static char rx_buf[RX_BUF_SIZE];

    for (;;) {
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);

        int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0,
                           (struct sockaddr *)&src_addr, &src_len);

        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom error: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Null-terminate */
        rx_buf[len] = '\0';

        /* Parse fields */
        device_state_t s = {0};
        s.valid         = true;
        s.uptime_ms     = (uint32_t)parse_int_field(rx_buf, "uptime");
        s.rssi          = (int)     parse_int_field(rx_buf, "rssi");
        s.stream_active = (bool)    parse_int_field(rx_buf, "stream_active");
        s.viewer_count  = (int)     parse_int_field(rx_buf, "viewer_count");
        s.reconnects    = (int)     parse_int_field(rx_buf, "reconnects");
        s.timestamp     = (uint32_t)parse_int_field(rx_buf, "timestamp");
        s.last_rx_tick  = esp_log_timestamp();

        /* Increment packet counter from current state */
        device_state_t prev = device_state_get();
        s.packet_count = prev.packet_count + 1;

        device_state_update(&s);

        /* ── Feed statistics engine ── */
        stats_sample_t stat_sample = {
            .rssi            = s.rssi,
            .heartbeat_rx_ms = s.last_rx_tick,
            .reconnects      = s.reconnects,
            .viewer_count    = s.viewer_count,
        };
        stats_engine_update(&stat_sample);

        /* ── Serial output ──
         * Rules for ESP-IDF v5 ESP_LOGI:
         *   - uint32_t  → PRIu32  (not %lu — the internal LOG_FORMAT already
         *                           injects a PRIu32 field, mixing %lu causes
         *                           the -Werror=format= failures seen in build)
         *   - int       → %d
         *   - IPSTR/IP2STR cannot be embedded in ESP_LOGI (IPSTR expands to
         *     "%d.%d.%d.%d" which breaks the macro's format-string stitching).
         *     Format the IP into a local buffer with inet_ntoa() instead.
         */
        char src_ip[16];
        strncpy(src_ip, inet_ntoa(src_addr.sin_addr), sizeof(src_ip) - 1);
        src_ip[sizeof(src_ip) - 1] = '\0';

        ESP_LOGI(TAG, "--- Heartbeat #%" PRIu32 " from %s ---",
                 s.packet_count, src_ip);
        ESP_LOGI(TAG, "  uptime   : %" PRIu32 " ms", s.uptime_ms);
        ESP_LOGI(TAG, "  rssi     : %d dBm",          s.rssi);
        ESP_LOGI(TAG, "  stream   : %s  viewers: %d",
                 s.stream_active ? "ACTIVE" : "idle", s.viewer_count);
        ESP_LOGI(TAG, "  reconnects: %d  cam-ts: %" PRIu32 " s",
                 s.reconnects, s.timestamp);

        /* Visual heartbeat confirmation — one LED blink per packet */
        led_blink_once();
    }

    close(sock);
    vTaskDelete(NULL);
}

void udp_receiver_start(void)
{
    xTaskCreate(udp_rx_task, "udp_rx", 4096, NULL, 5, NULL);
}
