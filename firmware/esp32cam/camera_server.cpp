/*
 * camera_server.cpp — ESP32-CAM
 *
 * Initialises the OV2640 camera and starts an HTTP server with two routes:
 *   GET /        — simple status page
 *   GET /stream  — MJPEG stream
 *
 * Architecture
 * ------------
 * The HTTP server runs in a dedicated FreeRTOS task pinned to core 1.
 * handleClient() loops there continuously, so the main task (core 0) is
 * never blocked.
 *
 * handle_stream() does NOT block inside the HTTP task. Instead it:
 *   1. Detaches the raw WiFiClient from the WebServer.
 *   2. Spawns a short-lived "streamer" task (core 1) that owns that client.
 *   3. Returns immediately so handleClient() stays live for other requests.
 *
 * The streamer task writes MJPEG frames in a tight loop, checks the client
 * connection after every frame, and cleans up atomically when the client
 * drops or an error occurs.
 *
 * Viewer state (s_viewer_count, s_stream_active) is protected by a mutex
 * so telemetry_collect() on core 0 never races the streamer on core 1.
 */

#include "camera_server.h"
#include "esp_camera.h"
#include <WebServer.h>
#include <WiFi.h>
#include <Arduino.h>

// AI Thinker ESP32-CAM pin map
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// Target frame interval. 10 fps is reliable on a local hotspot at VGA;
// raise to 15 fps (67 ms) if PSRAM is present and WiFi is strong.
#define FRAME_INTERVAL_MS  100

static WebServer          server(80);
static SemaphoreHandle_t  s_viewer_mutex  = nullptr;
static volatile int       s_viewer_count  = 0;
static volatile bool      s_stream_active = false;

// ── viewer state helpers (always call with mutex held) ────────────────────

static void viewer_inc() {
    if (xSemaphoreTake(s_viewer_mutex, portMAX_DELAY) == pdTRUE) {
        s_viewer_count++;
        s_stream_active = true;
        xSemaphoreGive(s_viewer_mutex);
    }
}

static void viewer_dec() {
    if (xSemaphoreTake(s_viewer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_viewer_count > 0) s_viewer_count--;
        if (s_viewer_count == 0) s_stream_active = false;
        xSemaphoreGive(s_viewer_mutex);
    }
}

// ── streamer task ─────────────────────────────────────────────────────────
// Each invocation owns one WiFiClient (heap-allocated, freed on exit).

static void streamer_task(void *arg) {
    WiFiClient *client = reinterpret_cast<WiFiClient *>(arg);

    // MJPEG multipart header
    client->println("HTTP/1.1 200 OK");
    client->println("Content-Type: multipart/x-mixed-replace; boundary=frame");
    client->println("Cache-Control: no-cache");
    client->println("Connection: close");
    client->println("Access-Control-Allow-Origin: *");
    client->println();

    viewer_inc();

    uint32_t last_frame_ms = 0;

    while (client->connected()) {
        // Pace frames — don't busy-spin.
        uint32_t now = millis();
        if (now - last_frame_ms < FRAME_INTERVAL_MS) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        last_frame_ms = now;

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[Camera] Frame capture failed — dropping frame.");
            // Brief pause before retry; avoids hammering the camera driver.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Build part header into a small stack buffer.
        char hdr[64];
        int  hdr_len = snprintf(hdr, sizeof(hdr),
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
            (unsigned)fb->len);

        bool ok = true;
        if (hdr_len > 0 && client->write((const uint8_t *)hdr, hdr_len) != (size_t)hdr_len) ok = false;
        if (ok && client->write(fb->buf, fb->len) != fb->len) ok = false;
        if (ok && client->write((const uint8_t *)"\r\n", 2) != 2) ok = false;

        esp_camera_fb_return(fb);

        if (!ok) {
            Serial.println("[Camera] Write failed — client likely disconnected.");
            break;
        }
    }

    viewer_dec();
    Serial.println("[Camera] Streamer task: client disconnected, cleaning up.");
    client->stop();
    delete client;
    vTaskDelete(nullptr);   // delete this task
}

// ── HTTP handlers ─────────────────────────────────────────────────────────

static void handle_root() {
    server.send(200, "text/plain",
        "TinyGuard ESP32-CAM\n"
        "Stream: http://192.168.137.10/stream\n");
}

static void handle_stream() {
    // Detach the client from the WebServer so it outlives this handler call.
    WiFiClient *client = new WiFiClient(server.client());
    server.client() = WiFiClient();   // give server an empty client

    if (!client || !client->connected()) {
        Serial.println("[Camera] handle_stream: client already gone.");
        delete client;
        return;
    }

    // Spawn the streamer on core 1 (same core as the HTTP task, avoids
    // cross-core cache pressure on the camera driver).
    BaseType_t res = xTaskCreatePinnedToCore(
        streamer_task,
        "cam_stream",
        4096,
        client,
        2,          // priority > HTTP task (1) so frames preempt handleClient
        nullptr,
        1
    );

    if (res != pdPASS) {
        Serial.println("[Camera] Failed to spawn streamer task.");
        client->stop();
        delete client;
    }
}

static void handle_not_found() {
    server.send(404, "text/plain", "Not found");
}

// ── Init ──────────────────────────────────────────────────────────────────

void camera_server_init() {
    s_viewer_mutex = xSemaphoreCreateMutex();
    if (!s_viewer_mutex) {
        Serial.println("[Camera] FATAL: could not create viewer mutex.");
        return;
    }

    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        config.frame_size   = FRAMESIZE_VGA;
        config.jpeg_quality = 10;
        config.fb_count     = 2;
        Serial.println("[Camera] PSRAM found — VGA / quality 10.");
    } else {
        config.frame_size   = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count     = 1;
        Serial.println("[Camera] No PSRAM — QVGA / quality 12.");
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[Camera] Init failed: 0x%x — check wiring.\n", err);
        return;
    }
    Serial.println("[Camera] Initialised.");

    server.on("/",       HTTP_GET, handle_root);
    server.on("/stream", HTTP_GET, handle_stream);
    server.onNotFound(handle_not_found);
    server.begin();
    Serial.println("[Camera] HTTP server started on port 80.");

    // HTTP server task — loops calling handleClient() on core 1.
    // Priority 1 (below streamer at 2) so active streaming preempts it.
    xTaskCreatePinnedToCore(
        [](void *) {
            for (;;) {
                server.handleClient();
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        },
        "http_server", 4096, nullptr, 1, nullptr, 1
    );
}

// ── Telemetry accessors (called from core 0 — mutex-protected) ───────────

bool camera_server_is_streaming() {
    bool v = false;
    if (s_viewer_mutex && xSemaphoreTake(s_viewer_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        v = s_stream_active;
        xSemaphoreGive(s_viewer_mutex);
    }
    return v;
}

int camera_server_get_viewer_count() {
    int v = 0;
    if (s_viewer_mutex && xSemaphoreTake(s_viewer_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        v = s_viewer_count;
        xSemaphoreGive(s_viewer_mutex);
    }
    return v;
}
