/*
 * fingerprint_engine.c — TinyGuard Monitor (Phase 2)
 *
 * Profile Divergence Score computation.
 * See fingerprint_engine.h for full design rationale.
 *
 * METRIC DRIFT COMPONENT
 * ----------------------
 * Uses behavior_profile_zscore() directly — that function already
 * handles the ready guard, the variance floor (ema_var < 0.25 → 0),
 * and the z = (current - ema_mean) / sqrt(ema_var) computation.
 * No need to reimplement it here.
 *
 * The four latest values come from a stats_engine_get_snapshot() call
 * made inside fingerprint_engine_update(). This is the only place in
 * Phase 2 that calls stats_engine_get_snapshot() outside udp_receiver —
 * acceptable because fingerprint_engine is the terminal consumer and
 * this avoids coupling the entire upstream pipeline to its needs.
 *
 * SESSION DRIFT COMPONENT
 * -----------------------
 * fingerprint_engine cannot z-score the current ongoing session duration
 * because session_tracker exposes completed session statistics only.
 * Two proxy signals are used instead:
 *
 *   1. Session count deviation:
 *      expected = window_duration_ms / ema_interval_ms
 *      z = (observed - expected) / sqrt(max(expected, 1))
 *      (Poisson-approximation stddev for count data)
 *
 *   2. Coefficient of variation (CV) on duration and interval:
 *      CV = stddev / mean. High CV = irregular session behavior.
 *      Scaled to z-score range: z_cv = CV * 3.0
 *      (CV = 1.0 maps to z = 3.0 = sub_score 50, which is appropriate
 *       — a stddev equal to the mean is meaningfully irregular)
 *
 * The session component takes the max of these signals.
 * This is a documented proxy. The correct long-term fix is to expose
 * current_session_elapsed_ms from session_tracker.
 *
 * WEIGHT REDISTRIBUTION
 * ---------------------
 * When session is not ready, the 25% weight does not simply disappear —
 * that would compress the PDS range (a score that should be 75 becomes
 * 56). Instead D+C are renormalized by dividing by 0.75, preserving
 * the full 0–100 range regardless of session readiness.
 */

#include "fingerprint_engine.h"
#include "stats_engine.h"
#include "alert_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "Fingerprint";

/* 60 heartbeats × 10 s = 600 s window */
#define WINDOW_DURATION_MS  600000.0f

/* ── internal state ──────────────────────────────────────────────────── */

typedef struct {
    fingerprint_snapshot_t last_snapshot;

    bool alert_elevated_active;
    int  consecutive_elevated;
    int  consecutive_normal_e;

    bool alert_critical_active;
    int  consecutive_critical;
    int  consecutive_normal_c;

    SemaphoreHandle_t mutex;
} fingerprint_state_t;

static fingerprint_state_t s_state;

/* ── helpers ─────────────────────────────────────────────────────────── */

static float fabs_f(float v) { return v < 0.0f ? -v : v; }

/* Linear ramp: |z| → 0–100 sub-score. */
static uint8_t z_to_subscore(float z_abs)
{
    float s = (z_abs / FINGERPRINT_Z_SCALE) * 100.0f;
    if (s < 0.0f)   s = 0.0f;
    if (s > 100.0f) s = 100.0f;
    return (uint8_t)s;
}

/* ── component scorers ───────────────────────────────────────────────── */

/*
 * D — Metric Drift
 * Uses behavior_profile_zscore() for each channel.
 * Latest values sourced from stats snapshot.
 */
static pds_component_t score_metric_drift(
        const behavior_profile_snapshot_t *bp,
        const stats_snapshot_t *ss)
{
    pds_component_t comp = { .ready = false };

    if (!bp->all_ready) return comp;
    comp.ready = true;

    float latest[4] = {
        ss->rssi.latest,
        ss->heartbeat_interval_ms.latest,
        ss->reconnect_rate.latest,
        ss->viewer_count.latest,
    };

    const long_term_stat_t *profiles[4] = {
        &bp->rssi,
        &bp->heartbeat_interval_ms,
        &bp->reconnect_rate,
        &bp->viewer_count,
    };

    float max_z = 0.0f;
    for (int i = 0; i < 4; i++) {
        /* behavior_profile_zscore returns 0 when not ready or flat */
        float z = fabs_f(behavior_profile_zscore(profiles[i], latest[i]));
        if (z > max_z) max_z = z;
    }

    comp.max_zscore = max_z;
    comp.sub_score  = z_to_subscore(max_z);
    return comp;
}

/*
 * C — Correlation Drift
 * zscore already computed per-pair inside correlation_tracker.
 */
static pds_component_t score_correlation_drift(
        const correlation_snapshot_t *cs)
{
    pds_component_t comp = { .ready = false };

    float max_z    = 0.0f;
    bool  any_ready = false;

    for (int i = 0; i < CORR_PAIR_COUNT; i++) {
        if (!cs->pairs[i].ready) continue;
        any_ready = true;
        float z = fabs_f(cs->pairs[i].zscore);
        if (z > max_z) max_z = z;
    }

    comp.ready      = any_ready;
    comp.max_zscore = any_ready ? max_z : 0.0f;
    comp.sub_score  = any_ready ? z_to_subscore(max_z) : 0;
    return comp;
}

/*
 * S — Session Drift
 * Four signals:
 *   1. Real-time: current session elapsed vs ema_duration (Phase 3)
 *   2. Session count deviation from expected frequency
 *   3. CV on session duration
 *   4. CV on inter-session interval
 */
static pds_component_t score_session_drift(
        const session_snapshot_t *se)
{
    pds_component_t comp = { .ready = false };

    if (!se->ready) return comp;
    comp.ready = true;

    float max_z = 0.0f;

    /* Signal 1: real-time current session duration z-score (Phase 3)
     * If currently streaming and ema_duration is established, score
     * how long the current session has been running vs the expected mean.
     * A session running 3× the normal duration is suspicious in real time,
     * not just after it ends. */
    if (se->in_session
        && se->current_session_elapsed_ms > 0
        && se->ema_duration_ms > 0.0f
        && se->ema_duration_stddev > 0.0f) {
        float z = fabs_f(((float)se->current_session_elapsed_ms
                          - se->ema_duration_ms)
                         / se->ema_duration_stddev);
        if (z > max_z) max_z = z;
    }

    /* Signal 2: session count deviation from expected frequency */
    if (se->ema_interval_ms > 0.0f) {
        float expected  = WINDOW_DURATION_MS / se->ema_interval_ms;
        float observed  = (float)se->session_count_in_window;
        float count_std = sqrtf(expected < 1.0f ? 1.0f : expected);
        float z         = fabs_f((observed - expected) / count_std);
        if (z > max_z) max_z = z;
    }

    /* Signal 3: CV on session duration */
    if (se->ema_duration_ms > 0.0f && se->ema_duration_stddev > 0.0f) {
        float cv = se->ema_duration_stddev / se->ema_duration_ms;
        float z  = cv * 3.0f;
        if (z > max_z) max_z = z;
    }

    /* Signal 4: CV on inter-session interval */
    if (se->ema_interval_ms > 0.0f && se->ema_interval_stddev > 0.0f) {
        float cv = se->ema_interval_stddev / se->ema_interval_ms;
        float z  = cv * 3.0f;
        if (z > max_z) max_z = z;
    }

    comp.max_zscore = max_z;
    comp.sub_score  = z_to_subscore(max_z);
    return comp;
}

/* ── PDS computation ─────────────────────────────────────────────────── */

static uint8_t compute_pds(const pds_component_t *d,
                            const pds_component_t *c,
                            const pds_component_t *s)
{
    float score = (float)d->sub_score * 0.40f
                + (float)c->sub_score * 0.35f;

    if (s->ready) {
        score += (float)s->sub_score * 0.25f;
    } else {
        /* Redistribute session weight: divide by combined D+C weight */
        score = score / 0.75f;
    }

    if (score < 0.0f)   score = 0.0f;
    if (score > 100.0f) score = 100.0f;
    return (uint8_t)score;
}

/* ── alert checks ────────────────────────────────────────────────────── */

static void check_alerts(uint8_t pds)
{
    /* Elevated */
    if (pds >= PDS_THRESH_ELEVATED) {
        s_state.consecutive_elevated++;
        s_state.consecutive_normal_e = 0;
        if (!s_state.alert_elevated_active
            && s_state.consecutive_elevated >= FINGERPRINT_CONSECUTIVE_THRESH) {
            s_state.alert_elevated_active = true;
            char msg[ALERT_MSG_LEN];
            snprintf(msg, sizeof(msg),
                     "PDS elevated: score=%u (threshold=%d)",
                     pds, PDS_THRESH_ELEVATED);
            alert_manager_raise(ALERT_LEVEL_WARNING, ALERT_TYPE_PDS_ELEVATED,
                                msg, (float)pds, (float)PDS_THRESH_ELEVATED,
                                0.0f, 0.0f);
        }
    } else {
        s_state.consecutive_normal_e++;
        s_state.consecutive_elevated = 0;
        if (s_state.alert_elevated_active
            && s_state.consecutive_normal_e >= FINGERPRINT_CONSECUTIVE_THRESH) {
            s_state.alert_elevated_active = false;
            ESP_LOGI(TAG, "PDS returned to normal (score=%u)", pds);
        }
    }

    /* Critical */
    if (pds >= PDS_THRESH_CRITICAL) {
        s_state.consecutive_critical++;
        s_state.consecutive_normal_c = 0;
        if (!s_state.alert_critical_active
            && s_state.consecutive_critical >= FINGERPRINT_CONSECUTIVE_THRESH) {
            s_state.alert_critical_active = true;
            char msg[ALERT_MSG_LEN];
            snprintf(msg, sizeof(msg),
                     "PDS critical: score=%u (threshold=%d)",
                     pds, PDS_THRESH_CRITICAL);
            alert_manager_raise(ALERT_LEVEL_CRITICAL, ALERT_TYPE_PDS_CRITICAL,
                                msg, (float)pds, (float)PDS_THRESH_CRITICAL,
                                0.0f, 0.0f);
        }
    } else {
        s_state.consecutive_normal_c++;
        s_state.consecutive_critical = 0;
        if (s_state.alert_critical_active
            && s_state.consecutive_normal_c >= FINGERPRINT_CONSECUTIVE_THRESH) {
            s_state.alert_critical_active = false;
            ESP_LOGI(TAG, "PDS dropped below critical (score=%u)", pds);
        }
    }
}

/* ── public API ──────────────────────────────────────────────────────── */

void fingerprint_engine_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG,
             "Initialised. z_scale=%.1f  elevated=%d  critical=%d  consecutive=%d",
             (double)FINGERPRINT_Z_SCALE,
             PDS_THRESH_ELEVATED, PDS_THRESH_CRITICAL,
             FINGERPRINT_CONSECUTIVE_THRESH);
}

void fingerprint_engine_update(void)
{
    behavior_profile_snapshot_t bp = behavior_profile_get_snapshot();
    if (!bp.all_ready) {
        /*
         * One-shot diagnostic: log once every ~60s while waiting, so it's
         * possible to confirm from serial whether behavior_profile is the
         * blocker, rather than this function silently never running.
         */
        static uint32_t s_last_wait_log_ms = 0;
        uint32_t now = esp_log_timestamp();
        if (now - s_last_wait_log_ms > 60000) {
            s_last_wait_log_ms = now;
            ESP_LOGI(TAG,
                     "Waiting on behavior_profile.all_ready "
                     "(rssi=%d hb=%d recon=%d view=%d)",
                     bp.rssi.ready, bp.heartbeat_interval_ms.ready,
                     bp.reconnect_rate.ready, bp.viewer_count.ready);
        }
        return;
    }

    stats_snapshot_t       ss = stats_engine_get_snapshot();
    correlation_snapshot_t cs = correlation_tracker_get_snapshot();
    session_snapshot_t     se = session_tracker_get_snapshot();

    pds_component_t d = score_metric_drift(&bp, &ss);
    pds_component_t c = score_correlation_drift(&cs);
    pds_component_t s = score_session_drift(&se);

    uint8_t pds = compute_pds(&d, &c, &s);

    static bool s_first_ready_logged = false;
    if (!s_first_ready_logged) {
        s_first_ready_logged = true;
        ESP_LOGI(TAG, "Fingerprint engine now ready — PDS active.");
    }

    ESP_LOGI(TAG, "PDS=%u  D=%u(z=%.2f)  C=%u(z=%.2f)  S=%u(z=%.2f)",
             pds,
             d.sub_score, (double)d.max_zscore,
             c.sub_score, (double)c.max_zscore,
             s.sub_score, (double)s.max_zscore);

    check_alerts(pds);

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.last_snapshot.pds               = pds;
    s_state.last_snapshot.metric_drift      = d;
    s_state.last_snapshot.correlation_drift = c;
    s_state.last_snapshot.session_drift     = s;
    s_state.last_snapshot.alert_elevated    = s_state.alert_elevated_active;
    s_state.last_snapshot.alert_critical    = s_state.alert_critical_active;
    s_state.last_snapshot.ready             = true;
    xSemaphoreGive(s_state.mutex);
}

fingerprint_snapshot_t fingerprint_engine_get_snapshot(void)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    fingerprint_snapshot_t out = s_state.last_snapshot;
    xSemaphoreGive(s_state.mutex);
    return out;
}
