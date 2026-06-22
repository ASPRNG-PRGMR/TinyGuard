#pragma once

/*
 * wifi_manager.h — TinyGuard Monitor
 *
 * Connects to the laptop hotspot with a static IP and blocks until connected.
 * Call wifi_manager_init() once from app_main() before starting other tasks.
 */

void wifi_manager_init(void);
