#pragma once

/*
 * session_tracker.h — TinyGuard Monitor (Phase 2)
 *
 * Session-level behavioral fingerprinting.
 *
 * PURPOSE
 * -------
 * behavior_profile and correlation_tracker operate on per-heartbeat metric
 * values. Neither captures event-level structure: how long the camera
 * streams, how frequently sessions start, how regular the pattern is.
 *
 * session_tracker models three session observables:
 *
 *   Session duration     — how long stream_active stays continuously true
 *   Inter-session interval — gap between stream end and next stream start
 *   Session count in window — how many sessions started in the last 60 HBs
 *
 * A camera that normally streams for ~5 minutes every ~30 minutes develops
 * a stable fingerprint in these values. An attacker probing the stream,
 * running it continuously, or suppressing it entirely will break the pattern
 * even when individual heartbeat metrics look normal.
 *
 * SIGNALS USED
 * ------------
 *   stream_active     — raw boolean from the heartbeat packet (not derived
 *                       from viewer_count)
 *   last_rx_tick_ms   — esp_log_timestamp() at packet receipt, used to
 *                       compute durations and intervals in wall-clock ms
 *
 * Reconnect behavior is explicitly excluded. It is already modeled by
 * stats_engine (reconnect_rate) and behavior_profile. Adding it here
 * would create feature overlap without detection value.
 *
 * STATE MACHINE
 * -------------
 * On each heartbeat, compare stream_active now vs previous heartbeat:
 *
 *   false → true  : session START. Record session_start_tick.
 *                   Set bit in shift register.
 *   true  → false : session END. Compute duration = now - session_start_tick.
 *                   Feed duration EMA. If a prior session has ended,
 *                   compute interval = session_start_tick - session_end_tick.
 *                   Feed interval EMA. Record session_end_tick.
 *   true  → true  : ongoing session — no event, no EMA update.
 *   false → false : idle — no event, no EMA update.
 *
 * The shift register advances one bit per heartbeat regardless of session
 * state. session_count_in_window is the popcount of the 64-bit register,
 * giving a rolling count of session starts over the last 60 heartbeats.
 *
 * ASSUMPTION — FIXED HEARTBEAT CADENCE
 * -------------------------------------
 * session_count_in_window uses a 64-bit shift register that advances one
 * bit per heartbeat. It assumes a fixed ~10-second heartbeat cadence.
 * At 10s/heartbeat the register covers 60 heartbeats = 600 seconds.
 * If the heartbeat interval changes significantly (missed packets, restart),
 * the window duration changes proportionally. This is documented and
 * acceptable for the current hardware setup.
 *
 * ALGORITHM — EMA
 * ---------------
 * Per-observable EMA mean and variance (Finch 2009), same estimator as
 * behavior_profile. Alpha = SESSION_ALPHA = 0.1 — faster than behavior_profile
 * (0.005) because sessions are infrequent events. At one session per hour
 * (360 heartbeats apart), alpha=0.005 would give a half-life of ~25 sessions
 * = 25 hours. Alpha=0.1 gives half-life ~7 sessions ≈ 7 hours — responsive
 * without being noisy.
 *
 * Seeding: EMA means are seeded from the first observed duration/interval
 * rather than zero-initialized, matching behavior_profile's seeding rationale.
 * A zero-initialized mean for duration would bias the EMA for many sessions.
 *
 * READINESS
 * ---------
 * ready = true when sessions_completed >= SESSION_MIN_SESSIONS (5).
 * No dependency on Phase 1 learning or behavior_profile.all_ready.
 * session_tracker accumulates observations at its own rate.
 * fingerprint_engine gates on session_tracker.ready independently.
 *
 * MEMORY
 * ------
 *   Internal state (session_tracker_t):
 *     bool × 4                    4 bytes (+ padding → ~8)
 *     uint32_t × 2 (ticks)        8 bytes
 *     float × 4 (EMA fields)     16 bytes
 *     bool × 2 (seeded flags)     2 bytes (+ padding → ~4)
 *     uint64_t shift register     8 bytes
 *     uint8_t session_count       1 byte  (+ padding → ~4)
 *     uint32_t sessions_completed 4 bytes
 *     SemaphoreHandle_t mutex     4 bytes
 *     Total (with alignment)    ~56 bytes static
 *
 *   session_snapshot_t (stack):  ~40 bytes per call
 *   No heap allocation.
 *
 * THREAD SAFETY
 * -------------
 * session_tracker_update() is called from udp_rx_task only.
 * session_tracker_get_snapshot() may be called from dashboard_server.
 * An internal mutex protects the snapshot copy — matching the pattern
 * of all other Phase 2 modules.
 *
 * INTEGRATION
 * -----------
 * Init:    session_tracker_init()       — called from app_main()
 * Update:  session_tracker_update()     — called from udp_rx_task after
 *                                         correlation_tracker_update()
 * Read:    session_tracker_get_snapshot() — called by fingerprint_engine
 *                                           and dashboard_server
 *
 * stream_active must be the raw boolean from the parsed heartbeat packet.
 * last_rx_tick_ms must be esp_log_timestamp() captured at packet receipt —
 * the same value stored in device_state.last_rx_tick.
 */

#include <stdint.h>
#include <stdbool.h>

/* ── tunables ─────────────────────────────────────────────────────────── */

/*
 * EMA decay factor for session duration and interval.
 * Faster than BEHAVIOR_PROFILE_ALPHA (0.005) because sessions are rare.
 * At alpha=0.1: half-life ≈ 7 sessions.
 */
#define SESSION_ALPHA               0.1f

/*
 * Sessions required before ready=true and EMA values are trusted.
 * 5 sessions gives the EMA enough observations to be meaningful without
 * requiring hours of operation before the fingerprint activates.
 */
#define SESSION_MIN_SESSIONS        5

/*
 * Minimum session duration in ms. Events shorter than this are treated
 * as noise (e.g. brief stream_active glitches) and discarded.
 * 2000 ms = 2 seconds.
 */
#define SESSION_MIN_DURATION_MS     2000

/* ── data structures ─────────────────────────────────────────────────── */

/*
 * Read-only snapshot returned to fingerprint_engine and dashboard_server.
 * All durations and intervals in milliseconds.
 */
typedef struct {
    float    ema_duration_ms;           /* mean session duration              */
    float    ema_duration_stddev;       /* stddev of session duration         */
    float    ema_interval_ms;           /* mean inter-session interval        */
    float    ema_interval_stddev;       /* stddev of inter-session interval   */
    uint8_t  session_count_in_window;   /* session starts in last 60 HBs      */
    bool     in_session;                /* stream currently active            */
    bool     ready;                     /* >= SESSION_MIN_SESSIONS completed  */
    uint32_t sessions_completed;        /* total completed sessions ever      */
    uint32_t current_session_elapsed_ms;/* ms since session start, 0 if idle */
} session_snapshot_t;

/* ── public API ───────────────────────────────────────────────────────── */

/* Call once from app_main() before udp_receiver_start(). */
void session_tracker_init(void);

/*
 * Ingest one heartbeat.
 * Called from udp_rx_task after correlation_tracker_update().
 *
 * stream_active   — raw boolean from the parsed heartbeat packet
 * last_rx_tick_ms — esp_log_timestamp() at packet receipt
 *                   (device_state.last_rx_tick)
 *
 * No readiness gating at this layer — session_tracker accumulates
 * observations regardless of Phase 1 or behavior_profile state.
 * fingerprint_engine gates on session_tracker.ready independently.
 */
void session_tracker_update(bool stream_active, uint32_t last_rx_tick_ms);

/*
 * Return a copy of current session state. Mutex-safe; may be called
 * from any task. Returns ready=false and zero EMA values until
 * sessions_completed >= SESSION_MIN_SESSIONS.
 */
session_snapshot_t session_tracker_get_snapshot(void);
