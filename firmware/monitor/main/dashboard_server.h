#pragma once

/*
 * dashboard_server.h — TinyGuard Monitor
 *
 * HTTP server on port 80.
 *
 * Routes:
 *   GET /        — Self-contained HTML/CSS/JS dashboard page.
 *                  Polls /status every 10 seconds via fetch().
 *                  Updates the DOM in place — no page refresh.
 *                  Accumulates rolling metric history client-side
 *                  and renders sparklines on <canvas> elements.
 *
 *   GET /status  — JSON snapshot consumed by the dashboard JS and
 *                  available for curl / scripting.
 *
 * Phase 2 panels exposed on the dashboard:
 *   Device Health     — status badge, PDS badge, heartbeat age, RSSI,
 *                       stream state, viewers, reconnects, packets
 *   Behavioral FP     — PDS score + bar, D/C/S component breakdown
 *   Correlation       — per-pair r, EMA r, z-score, alert indicator
 *   Session Behavior  — duration/interval EMA ± stddev, session count
 *   Statistics        — mean/stddev + live sparkline per metric
 *   Alert Timeline    — last 8 alerts, source-categorized
 *
 * The page is served from flash (const char[] PAGE_HTML in the .c file).
 * No SPIFFS, no filesystem, no external assets.
 * The JSON buffer (3072 bytes) is stack-allocated per request.
 *
 * Call dashboard_server_start() once from app_main() after WiFi is up.
 */

void dashboard_server_start(void);
