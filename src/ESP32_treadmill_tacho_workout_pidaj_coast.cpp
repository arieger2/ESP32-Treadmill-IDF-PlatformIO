#include <Arduino.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout_pidaj_ctx.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;

void pidajHandleCoast(PidajCtx& ctx, const PidajInputs& in, const PidajModel& m) {
    const uint32_t coastElapsed = in.now - ctx.stateTime;

    uint32_t dynamicMinCoastMs = storedGlobals.COAST_NEAR_MIN_MS;
    if (in.errorAbs >= COAST_ERROR_FAR_KMH) {
        dynamicMinCoastMs = storedGlobals.COAST_FAR_MIN_MS;
    } else if (in.errorAbs > COAST_ERROR_NEAR_KMH) {
        const float t = (in.errorAbs - COAST_ERROR_NEAR_KMH) /
                        (COAST_ERROR_FAR_KMH - COAST_ERROR_NEAR_KMH);
        dynamicMinCoastMs = (uint32_t)((float)storedGlobals.COAST_NEAR_MIN_MS
                             - t * (float)(storedGlobals.COAST_NEAR_MIN_MS - storedGlobals.COAST_FAR_MIN_MS));
    }

    uint32_t responseAwareMinMs = m.responseDelayMs / 2;
    if (responseAwareMinMs < 150) responseAwareMinMs = 150;
    if (responseAwareMinMs > 800) responseAwareMinMs = 800;

    const uint32_t minCoastMs = (dynamicMinCoastMs > responseAwareMinMs) ? dynamicMinCoastMs : responseAwareMinMs;
    uint32_t effectiveMinCoastMs = minCoastMs;
    if (ctx.pendingModelCatchupMs > 0) {
        uint32_t modelCatchupMinMs = m.responseDelayMs + ctx.pendingModelCatchupMs;
        if (modelCatchupMinMs > MAX_MODEL_CATCHUP_MS) modelCatchupMinMs = MAX_MODEL_CATCHUP_MS;
        if (modelCatchupMinMs > effectiveMinCoastMs)  effectiveMinCoastMs = modelCatchupMinMs;
    }

    const bool farFromTarget  = (in.errorAbs >= COAST_FAST_RESUME_ERROR_KMH);
    const bool settledByAccel = (fabsf(metrics.acceleration) < storedGlobals.PID_COAST_THRESHOLD);
    const bool settled        = (coastElapsed >= effectiveMinCoastMs) && (farFromTarget || settledByAccel);

    uint32_t timeoutMs = storedGlobals.INERTIA_DELAY_MS + COAST_TIMEOUT_EXTRA_MS;
    const uint32_t modelAwareTimeoutMs = effectiveMinCoastMs + COAST_TIMEOUT_EXTRA_MS;
    if (modelAwareTimeoutMs > timeoutMs) timeoutMs = modelAwareTimeoutMs;
    const bool timeout = (coastElapsed >= timeoutMs);

    if (settled || timeout) {
        ctx.integral            = 0.0f;
        ctx.inErrorBand         = (in.errorAbs <= m.bandExit);
        ctx.pendingModelCatchupMs = 0;
        ctx.state               = PIDAJ_IDLE;
        Serial.printf("[PIDAJ] Coast done at %.1f km/h after %ums (min=%u, target %.1f, err=%.2f, fast=%d)\n",
                      in.speed, coastElapsed, effectiveMinCoastMs, in.targetSpeed_kmh, in.error,
                      farFromTarget ? 1 : 0);
    }
}
