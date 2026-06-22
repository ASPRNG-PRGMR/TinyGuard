#pragma once

void wifi_manager_init();
const char* wifi_manager_get_ip();
int wifi_manager_get_rssi();
int wifi_manager_get_reconnect_count();
