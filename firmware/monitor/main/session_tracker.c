/*
 * session_tracker.c — TinyGuard Monitor (Phase 2)
 *
 * Session-level behavioral fingerprinting.
 * See session_tracker.h for full design rationale.
 *
 * MUTEX STRATEGY
 * --------------
 * session_tracker_update() is called only from udp_rx_task — it is the
 * sole writer. However, get_snapshot() may be called concurrently from
 * dashboard_server (HTTP handler task). A reader could observe a partially
 * committed update: e.g., in_session=false with ema_duration_ms not yet
 * updated, producing a misleading snapshot.
 *
 * Resolution: the state machine computation (duration delta, interval delta)
 * runs outside the mutex since it only reads local variables and tick values.
 * All writes to shared state (EMA fields, sessions_completed, in_session,
 * session_end_tick, ready) are committed inside the mutex in a single block
 * at the end of the update. get_snapshot() always sees a consistent state.
 *
 * EMA SEEDING
 * -----------
 * On the first valid completed session, ema_duration_ms is set directly
 * to the observed value rather than updated from zero. Matches
 * behavior_profile's seeding rationale — zero-start bias is severe for
 * infrequent events with large values (e.g. 300,000 ms session duration).
 * Same pattern for ema_interval_ms on the first completed interval.
 *
 * SHIFT REGISTER
 * --------------
 * session_start_window is a uint64_t shift register. Advanced one position
 * per heartbeat (left shift). Bit 0 set on session start events.
 * session_count = __builtin_popcountll(session_start_window).
 * Covers 64 heartbeats; only 60 are active (STATS_WINDOW_SIZE). The upper
 * 4 bits age out naturally and never carry a set bit from within the window.
 *
 * DURATION FLOOR
 * --------------
 * Sessions shorter than SESSION_MIN_DURATION_MS are discarded — no EMA
 * update, no sessions_completed increment. Prevents stream_active glitches
 * from corrupting the duration baseline.
 */

#include "session_tracker.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "SessionTracker";

/* ── internal state ──────────────────────────────────────────────────── */

typedef struct {
    /* State machine */
    bool     stream_was_active;
    bool     in_session;
    bool     has_prior_end;
    uint32_t session_start_tick;
    uint32_t session_end_tick;

    /* EMA — duration */
    float    ema_duration_ms;
    float    ema_duration_var;
    bool     duration_seeded;

    /* EMA — inter-session interval */
    float    ema_interval_ms;
    float    ema_interval_var;
    bool     interval_seeded;

    /* Rolling window — session count */
    uint64_t session_start_window;
    uint8_t  session_count;

    /* Readiness */
    uint32_t sessions_completed;
    bool     ready;

    SemaphoreHandle_t mutex;
} session_state_t;

static session_state_t s_state;

/* ── EMA helpers ─────────────────────────────────────────────────────── */

static void ema_seed(float *mean, float *var, bool *seeded, float x)
{
    *mean   = x;
    *var    = 0.0f;
    *seeded = true;
}

static void ema_update_val(float *mean, float *var, float x)
{
    float diff = x - *mean;
    *mean     += SESSION_ALPHA * diff;
    *var       = (1.0f - SESSION_ALPHA)
               * (*var + SESSION_ALPHA * diff * diff);
}

/* ── public API ──────────────────────────────────────────────────────── */

void session_tracker_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG,
             "Initialised. alpha=%.2f  min_sessions=%d  min_duration_ms=%d",
             (double)SESSION_ALPHA,
             SESSION_MIN_SESSIONS,
             SESSION_MIN_DURATION_MS);
}

void session_tracker_update(bool stream_active, uint32_t last_rx_tick_ms)
{
    bool was = s_state.stream_was_active;

    /*
     * Compute all deltas before taking the mutex.
     * Only reads local/argument values — no shared state read here.
     */
    bool    session_started  = (!was && stream_active);
    bool    session_ended    = (was && !stream_active);
    uint32_t duration_ms     = 0;
    bool    duration_valid   = false;
    uint32_t interval_ms     = 0;
    bool    interval_valid   = false;

    if (session_ended) {
        duration_ms   = last_rx_tick_ms - s_state.session_start_tick;
        duration_valid = (duration_ms >= (uint32_t)SESSION_MIN_DURATION_MS);

        if (duration_valid && s_state.has_prior_end) {
            interval_ms   = s_state.session_start_tick - s_state.session_end_tick;
            interval_valid = true;
        }
    }

    /* ── Commit all state changes inside the mutex ── */
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    /* Advance shift register every heartbeat */
    s_state.session_start_window <<= 1;

    if (session_started) {
        s_state.in_session         = true;
        s_state.session_start_tick = last_rx_tick_ms;
        s_state.session_start_window |= 1ULL;
        ESP_LOGD(TAG, "Session started at %" PRIu32 " ms", last_rx_tick_ms);
    }

    if (session_ended) {
        s_state.in_session = false;

        if (!duration_valid) {
            ESP_LOGD(TAG,
                     "Session discarded: duration %" PRIu32 " ms < min %d ms",
                     duration_ms, SESSION_MIN_DURATION_MS);
        } else {
            /* Update duration EMA */
            if (!s_state.duration_seeded) {
                ema_seed(&s_state.ema_duration_ms,
                         &s_state.ema_duration_var,
                         &s_state.duration_seeded,
                         (float)duration_ms);
            } else {
                ema_update_val(&s_state.ema_duration_ms,
                               &s_state.ema_duration_var,
                               (float)duration_ms);
            }

            /* Update interval EMA */
            if (interval_valid) {
                if (!s_state.interval_seeded) {
                    ema_seed(&s_state.ema_interval_ms,
                             &s_state.ema_interval_var,
                             &s_state.interval_seeded,
                             (float)interval_ms);
                } else {
                    ema_update_val(&s_state.ema_interval_ms,
                                   &s_state.ema_interval_var,
                                   (float)interval_ms);
                }
            }

            s_state.session_end_tick = last_rx_tick_ms;
            s_state.has_prior_end    = true;
            s_state.sessions_completed++;

            if (!s_state.ready
                && s_state.sessions_completed >= SESSION_MIN_SESSIONS) {
                s_state.ready = true;
                ESP_LOGI(TAG,
                         "Session profile ready after %" PRIu32 " sessions. "
                         "ema_duration=%.0f ms  ema_interval=%.0f ms",
                         s_state.sessions_completed,
                         (double)s_state.ema_duration_ms,
                         (double)s_state.ema_interval_ms);
            }

            ESP_LOGD(TAG,
                     "Session ended: duration=%" PRIu32 " ms  "
                     "ema_duration=%.0f ms  sessions=%" PRIu32,
                     duration_ms,
                     (double)s_state.ema_duration_ms,
                     s_state.sessions_completed);
        }
    }

    s_state.session_count     = (uint8_t)__builtin_popcountll(
                                    s_state.session_start_window);
    s_state.stream_was_active = stream_active;

    xSemaphoreGive(s_state.mutex);
}

session_snapshot_t session_tracker_get_snapshot(void)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    session_snapshot_t out;
    out.ema_duration_ms       = s_state.ema_duration_ms;
    out.ema_duration_stddev   = (s_state.ema_duration_var > 0.0f)
                                ? sqrtf(s_state.ema_duration_var) : 0.0f;
    out.ema_interval_ms       = s_state.ema_interval_ms;
    out.ema_interval_stddev   = (s_state.ema_interval_var > 0.0f)
                                ? sqrtf(s_state.ema_interval_var) : 0.0f;
    out.session_count_in_window    = s_state.session_count;
    out.in_session                 = s_state.in_session;
    out.ready                      = s_state.ready;
    out.sessions_completed         = s_state.sessions_completed;
    out.current_session_elapsed_ms = s_state.in_session
        ? (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS
                     - s_state.session_start_tick)
        : 0;

    xSemaphoreGive(s_state.mutex);
    return out;
}
