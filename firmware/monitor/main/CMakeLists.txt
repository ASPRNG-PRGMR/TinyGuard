idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "device_state.c"
        "udp_receiver.c"
        "stats_engine.c"
        "fingerprint_engine.c"
        "anomaly_engine.c"
        "alert_manager.c"
        "dashboard_server.c"
        "behavior_profile.c"
        "correlation_tracker.c"
        "session_tracker.c"
    INCLUDE_DIRS "."
    REQUIRES mdns
    PRIV_REQUIRES
        esp_wifi
        nvs_flash
        esp_netif
        esp_event
        esp_driver_gpio
        esp_http_server
        lwip
)
# sqrtf() / fabsf() from libm
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
