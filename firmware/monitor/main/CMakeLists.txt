idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "device_state.c"
        "udp_receiver.c"
        "stats_engine.c"
    INCLUDE_DIRS "."
)

# sqrtf() is in libm — required by stats_engine.c
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
