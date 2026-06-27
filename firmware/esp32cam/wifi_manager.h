#pragma once

void        wifi_manager_init();
bool        wifi_manager_resolve_monitor_ip();  // call once after init
const char* wifi_manager_get_monitor_ip();      // resolved monitor address
const char* wifi_manager_get_ip();
int         wifi_manager_get_rssi();
int         wifi_manager_get_reconnect_count();
