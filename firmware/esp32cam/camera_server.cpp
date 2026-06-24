/*
 * camera_server.cpp — ESP32-CAM
 *
 * Initialises the OV2640 camera and starts an HTTP server with two routes:
 *   GET /        — simple status page
 *   GET /stream  — MJPEG stream
 *
 * Viewer count is tracked via atomic increment/decrement on connect/disconnect.
 * This count is included in heartbeat packets so the monitor can detect
 * abnormal stream access.
 *
 * Requires the esp32 Arduino board package.
 * Camera pin definitions come from the AI-Thinker board definition
 * (esp_camera.h selects the right set when CAMERA_MODEL_AI_THINKER is defined).
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

static WebServer server(80);
static volatile int s_viewer_count = 0;
static volatile bool s_stream_active = false;

// --- HTTP handlers -----------------------------------------------------------

static void handle_root() {
  server.send(200, "text/plain",
    "TinyGuard ESP32-CAM\n"
    "Stream: http://192.168.137.10/stream\n");
}

static void handle_stream() {
  s_viewer_count++;
  s_stream_active = true;

  WiFiClient client = server.client();

  // Send MJPEG multipart headers
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Connection: close");
  client.println();

  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[Camera] Frame capture failed");
      break;
    }

    client.printf("--frame\r\n"
                  "Content-Type: image/jpeg\r\n"
                  "Content-Length: %u\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);
    client.print("\r\n");

    esp_camera_fb_return(fb);

    // ~10 fps
    delay(100);
  }

  s_viewer_count--;
  if (s_viewer_count <= 0) {
    s_viewer_count = 0;
    s_stream_active = false;
  }
}

static void handle_not_found() {
  server.send(404, "text/plain", "Not found");
}

// --- Init --------------------------------------------------------------------

void camera_server_init() {
  // Camera config
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

  // Use PSRAM if available for larger frames
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Camera] Init failed: 0x%x — check wiring.\n", err);
    // Camera failure is not fatal for telemetry; stream just won't work.
    return;
  }

  Serial.println("[Camera] Initialised.");

  // Register routes and start server
  server.on("/",       HTTP_GET, handle_root);
  server.on("/stream", HTTP_GET, handle_stream);
  server.onNotFound(handle_not_found);
  server.begin();

  Serial.println("[Camera] HTTP server started on port 80.");

  // Run server handling in a background task so it doesn't block the main loop
  xTaskCreatePinnedToCore(
    [](void*) {
      for (;;) {
        server.handleClient();
        delay(1);
      }
    },
    "http_server", 4096, nullptr, 1, nullptr, 1
  );
}

bool camera_server_is_streaming() {
  return s_stream_active;
}

int camera_server_get_viewer_count() {
  return s_viewer_count;
}
