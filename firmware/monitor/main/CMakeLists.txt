idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "device_state.c"
        "udp_receiver.c"
        "stats_engine.c"
        "anomaly_engine.c"
        "alert_manager.c"
        "dashboard_server.c"
        "behavior_profile.c"
	"correlation_tracker.c"
    	INCLUDE_DIRS "."
)

# sqrtf() / fabsf() from libm
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
