/*
 * wifi_manager.c — TinyGuard Monitor (ESP-IDF)
 *
 * Connects to the hotspot using DHCP. No static IP.
 *
 * The monitor announces itself via mDNS as "tinyguard-monitor.local" so
 * the dashboard is reachable by name regardless of what subnet the hotspot
 * assigns. This works on any hotspot (Windows, Fedora, Android, iOS)
 * without reflashing when the network changes.
 *
 * Why DHCP instead of static IP
 * ------------------------------
 * The previous static IP (192.168.137.20) only worked on the Windows
 * laptop hotspot, which uses that subnet by default. Fedora's NetworkManager
 * assigns 10.42.0.x, Android hotspots use 192.168.43.x, and so on. With a
 * static IP the ESP connects to the AP (WiFi association works independently
 * of IP configuration) but ends up with an address on a different subnet
 * from the host — so HTTP and UDP are unreachable even though WiFi is up.
 * DHCP eliminates this entirely: the AP assigns a compatible address.
 *
 * mDNS replaces hardcoded IPs for cross-device addressing:
 *   Dashboard : http://tinyguard-monitor.local/
 *   Status    : http://tinyguard-monitor.local/status
 *   Camera    : http://tinyguard-cam.local/stream
 *
 * The camera node resolves tinyguard-monitor.local to send heartbeats.
 * The monitor does not need to know the camera's IP — it just listens
 * on UDP 5000 and accepts packets from any source.
 */

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

/* ── configuration ───────────────────────────────────────────────────── */
#define WIFI_SSID          "idk"
#define WIFI_PASSWORD      "lol12345"

#define MDNS_HOSTNAME      "tinyguard-monitor"
#define MDNS_INSTANCE      "TinyGuard Monitor"

#define CONNECT_TIMEOUT_MS  20000
/* ──────────────────────────────────────────────────────────────────────── */

static const char        *TAG = "WiFi-Monitor";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected — reconnecting...");
        esp_wifi_connect();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected. IP: " IPSTR "  (DHCP)",
                 IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── mDNS helper ─────────────────────────────────────────────────────── */

static void mdns_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set(MDNS_INSTANCE);

    /* Advertise HTTP so network scanners can find the dashboard */
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS: http://%s.local/", MDNS_HOSTNAME);
}

/* ── public API ──────────────────────────────────────────────────────── */

void wifi_manager_init(void)
{
    /* NVS required by WiFi driver */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* DHCP — use the default STA netif, do NOT stop dhcpc or set static IP */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,   &event_handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,  IP_EVENT_STA_GOT_IP, &event_handler, NULL, &h_ip));

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid,     WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s (DHCP) ...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT,
        pdFALSE, pdTRUE,
        pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Connection timed out — restarting.");
        esp_restart();
    }

    /* Start mDNS after IP is confirmed */
    mdns_start();
}
