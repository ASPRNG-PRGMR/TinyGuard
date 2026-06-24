#pragma once

/*
 * dashboard_server.h — TinyGuard Monitor
 *
 * HTTP server on port 80 serving a single-page dashboard.
 * Auto-refreshes every 2 seconds via meta refresh.
 *
 * Routes:
 *   GET /        — full HTML dashboard page
 *   GET /status  — JSON snapshot (device state + stats + recent alerts)
 *
 * The JSON endpoint exists for future use (Phase 2 tooling, curl checks).
 * The HTML page is self-contained: no external CDN, no JavaScript frameworks.
 * All styling is inline CSS. Renders correctly on mobile.
 *
 * Call dashboard_server_start() once from app_main() after WiFi is up.
 */

void dashboard_server_start(void);
