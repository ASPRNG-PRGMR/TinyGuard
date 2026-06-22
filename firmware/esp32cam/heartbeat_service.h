#pragma once

void heartbeat_service_init();

/*
 * Call from loop(). Sends a UDP heartbeat every HEARTBEAT_INTERVAL_MS.
 * Non-blocking — tracks its own timer internally.
 */
void heartbeat_service_tick();
