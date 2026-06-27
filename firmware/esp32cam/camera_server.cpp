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
 *
 * Power / stability notes
 * -----------------------
 * The AI Thinker ESP32-CAM draws up to ~300 mA during WiFi TX bursts.
 * With the camera sensor clocking at 20 MHz this peaks higher and causes
 * brownout resets on resistive USB cables (appearing as jitter or sudden
 * shutdown after a few minutes of streaming).
 *
 * Mitigations applied here:
 *   - XCLK reduced from 20 MHz to 16 MHz. Lowers sensor current draw;
 *     the OV2640 datasheet specifies 6–27 MHz — 16 is well within range.
 *   - fb_count forced to 1 regardless of PSRAM. Double-buffering fills
 *     PSRAM faster and increases peak current; single-buffer returns RAM
 *     immediately after each frame write.
 *   - FRAME_INTERVAL_MS set to 120 ms (~8 fps). Reduces WiFi TX duty
 *     cycle — the dominant source of current spikes — without making the
 *     stream noticeably worse for a fixed monitoring camera.
 *   - A 10 ms vTaskDelay after each successful frame write lets the WiFi
 *     stack drain its TX queue before the next frame is fetched, smoothing
 *     the current envelope.
 *
 * Image orientation
 * -----------------
 * The camera is mounted inverted (board slot between PCB and module).
 * HMIRROR + VFLIP applied via sensor registers after init so the stream
 * appears correctly oriented in the browser without any client-side transform.
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

// ~8 fps. Reduces WiFi TX duty cycle (main source of current spikes).
// Raise to 100 (10 fps) if your cable + power supply can handle it.
#define FRAME_INTERVAL_MS  120

static WebServer          server(80);
static SemaphoreHandle_t  s_viewer_mutex  = nullptr;
static volatile int       s_viewer_count  = 0;
static volatile bool      s_stream_active = false;

// ── viewer state helpers ──────────────────────────────────────────────────

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

static void streamer_task(void *arg) {
    WiFiClient *client = reinterpret_cast<WiFiClient *>(arg);

    // Use explicit \r\n throughout — Firefox's multipart parser is strict
    // about CRLF line endings in both the HTTP response headers and the
    // MIME part headers. Arduino's println() sends \r\n on most builds but
    // is not guaranteed; explicit literals are safe on all board packages.
    static const char *resp_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    client->write((const uint8_t *)resp_hdr, strlen(resp_hdr));

    viewer_inc();

    uint32_t last_frame_ms = 0;
    bool     first_frame   = true;

    while (client->connected()) {
        uint32_t now = millis();
        if (now - last_frame_ms < FRAME_INTERVAL_MS) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        last_frame_ms = now;

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[Camera] Frame capture failed — dropping frame.");
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        // RFC 2046: boundary delimiter is \r\n--boundary for all parts
        // except the very first, which has no leading \r\n.
        char hdr[96];
        int  hdr_len = snprintf(hdr, sizeof(hdr),
            "%s--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n"
            "\r\n",
            first_frame ? "" : "\r\n",
            (unsigned)fb->len);
        first_frame = false;

        bool ok = true;
        if (hdr_len > 0 && client->write((const uint8_t *)hdr, hdr_len) != (size_t)hdr_len) ok = false;
        if (ok && client->write(fb->buf, fb->len) != fb->len) ok = false;

        esp_camera_fb_return(fb);

        if (!ok) {
            Serial.println("[Camera] Write failed — client likely disconnected.");
            break;
        }

        // Let WiFi TX queue drain before fetching the next frame.
        // Smooths the current envelope; reduces brownout risk.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    viewer_dec();
    Serial.println("[Camera] Streamer task: client disconnected, cleaning up.");
    client->stop();
    delete client;
    // Give the TCP stack time to fully release the socket before this task
    // exits. Without this delay, rapid refreshes hit ERR_ADDRESS_UNREACHABLE
    // because the WebServer's connection slot isn't free yet when the next
    // SYN arrives.
    vTaskDelay(pdMS_TO_TICKS(200));
    vTaskDelete(nullptr);
}

// ── HTTP handlers ─────────────────────────────────────────────────────────

static void handle_root() {
    char buf[192];
    snprintf(buf, sizeof(buf),
        "TinyGuard ESP32-CAM\n"
        "View   : http://%s/view   (all browsers)\n"
        "Stream : http://%s/stream (Chrome/Brave only)\n"
        "mDNS   : http://tinyguard-cam.local/view\n",
        WiFi.localIP().toString().c_str(),
        WiFi.localIP().toString().c_str());
    server.send(200, "text/plain", buf);
}

static void handle_view() {
    // Minimal HTML wrapper for Firefox/Linux compatibility.
    // Firefox does not render raw multipart/x-mixed-replace streams on direct
    // navigation — it treats them as file downloads. Embedding the stream in
    // an <img> tag inside an HTML document renders correctly in all browsers.
    char buf[512];
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head>"
        "<title>TinyGuard CAM</title>"
        "<style>"
          "body{margin:0;background:#0d1117;display:flex;align-items:center;"
                "justify-content:center;height:100vh;}"
          "img{max-width:100%%;max-height:100vh;display:block;}"
        "</style>"
        "</head><body>"
        "<img src=\"http://%s/stream\" />"
        "</body></html>",
        WiFi.localIP().toString().c_str());
    server.send(200, "text/html", buf);
}

static void handle_stream() {
    WiFiClient *client = new WiFiClient(server.client());
    server.client() = WiFiClient();

    if (!client || !client->connected()) {
        Serial.println("[Camera] handle_stream: client already gone.");
        delete client;
        return;
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        streamer_task,
        "cam_stream",
        4096,
        client,
        2,
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

    // 16 MHz instead of 20 MHz — reduces sensor current draw.
    // OV2640 supports 6–27 MHz; image quality is unaffected.
    config.xclk_freq_hz = 16000000;
    config.pixel_format = PIXFORMAT_JPEG;

    // fb_count=1 always. Double-buffering increases peak current;
    // single-buffer is sufficient for 8 fps and safer on marginal USB power.
    if (psramFound()) {
        config.frame_size   = FRAMESIZE_VGA;
        config.jpeg_quality = 10;
        config.fb_count     = 1;
        Serial.println("[Camera] PSRAM found — VGA / quality 10 / fb_count 1.");
    } else {
        config.frame_size   = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count     = 1;
        Serial.println("[Camera] No PSRAM — QVGA / quality 12 / fb_count 1.");
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[Camera] Init failed: 0x%x — check wiring.\n", err);
        return;
    }

    // Apply image orientation correction via sensor registers.
    // The module is mounted inverted (slotted between PCB and ESP-CAM module),
    // so both horizontal mirror and vertical flip are needed.
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_hmirror(s, 1);
        s->set_vflip(s, 1);
        Serial.println("[Camera] Orientation: HMIRROR + VFLIP applied.");
    } else {
        Serial.println("[Camera] Warning: could not get sensor handle for orientation.");
    }

    Serial.println("[Camera] Initialised.");

    server.on("/",       HTTP_GET, handle_root);
    server.on("/view",   HTTP_GET, handle_view);
    server.on("/stream", HTTP_GET, handle_stream);
    server.onNotFound(handle_not_found);
    server.begin();
    Serial.println("[Camera] HTTP server started on port 80.");
    Serial.printf("[Camera] View   : http://%s/view   (all browsers)\n", WiFi.localIP().toString().c_str());
    Serial.printf("[Camera] Stream : http://%s/stream (Chrome/Brave)\n", WiFi.localIP().toString().c_str());
    Serial.println("[Camera] mDNS  : http://tinyguard-cam.local/view");

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

// ── Telemetry accessors ───────────────────────────────────────────────────

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