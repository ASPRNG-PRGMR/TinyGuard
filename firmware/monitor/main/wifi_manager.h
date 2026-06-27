#pragma once

/*
 * wifi_manager.h — TinyGuard Monitor
 *
 * Connects to the hotspot via DHCP and announces the monitor as
 * "tinyguard-monitor.local" via mDNS.
 *
 * Call wifi_manager_init() once from app_main() before starting other tasks.
 * Blocks until IP is acquired (20s timeout → restart).
 *
 * After init, the dashboard is reachable at:
 *   http://tinyguard-monitor.local/
 *   http://tinyguard-monitor.local/status
 */

void wifi_manager_init(void);
