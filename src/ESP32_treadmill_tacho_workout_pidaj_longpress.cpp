#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout_pidaj_ctx.h"

extern TreadmillStoredGlobals storedGlobals;

void pidajHandleLongPress(PidajCtx& ctx, const PidajInputs& in, const PidajModel& m) {
    const uint32_t pressElapsed  = in.now - ctx.pressStartTime;
    const float    beltRate      = ctx.goingUp ? m.beltRateUp   : m.beltRateDown;
    const float    inertiaKmh    = ctx.goingUp ? m.inertiaUpKmh : m.inertiaDownKmh;
    const float    responseDelayS = m.responseDelayMs / 1000.0f;

    float predictionHorizonS = storedGlobals.INERTIA_DELAY_MS / 1000.0f;
    if (predictionHorizonS < PREDICTION_HORIZON_MIN_S) predictionHorizonS = PREDICTION_HORIZON_MIN_S;
    if (predictionHorizonS > PREDICTION_HORIZON_MAX_S) predictionHorizonS = PREDICTION_HORIZON_MAX_S;

    const float predicted = in.speed
                          + in.accel * predictionHorizonS
                          + 0.5f * in.jrk * predictionHorizonS * predictionHorizonS;
    const float releaseGuardKmh = inertiaKmh + beltRate * responseDelayS * 0.25f;
    const float releaseSpeed    = ctx.goingUp ? (in.targetSpeed_kmh - releaseGuardKmh)
                                              : (in.targetSpeed_kmh + releaseGuardKmh);

    const bool releaseBySpeed      = ctx.goingUp ? (in.speed >= releaseSpeed)          : (in.speed <= releaseSpeed);
    const bool releaseByPrediction = ctx.goingUp ? (predicted >= in.targetSpeed_kmh)   : (predicted <= in.targetSpeed_kmh);
    const bool releaseByCrossing   = ctx.goingUp ? (in.error <= 0.0f)                  : (in.error >= 0.0f);
    const bool releaseByPlan       = (ctx.plannedPressMs > 0) && (pressElapsed >= ctx.plannedPressMs);
    const bool minHoldReached      = (pressElapsed >= MIN_RELAY_ON_MS);
    const bool farFromTarget       = (in.errorAbs >= FORCE_LONG_PRESS_ERROR_KMH);

    uint32_t earlyReleaseAfterMs = MIN_RELAY_ON_MS;
    if (ctx.plannedPressMs > 0) {
        const uint32_t gate = (uint32_t)((float)ctx.plannedPressMs * EARLY_RELEASE_PLAN_FRACTION);
        if (gate > earlyReleaseAfterMs) earlyReleaseAfterMs = gate;
    }
    const bool earlyReleaseWindowReached = (pressElapsed >= earlyReleaseAfterMs);

    bool release = releaseByPlan;
    if (!farFromTarget && minHoldReached && earlyReleaseWindowReached)
        release = release || (releaseBySpeed || releaseByPrediction || releaseByCrossing);

    if (pressElapsed >= storedGlobals.PID_LONG_PRESS_MAX_MS) {
        release = true;
        Serial.printf("[PIDAJ] Safety timeout after %us\n", pressElapsed / 1000);
    }

    if (release) {
        ctx.pendingModelCatchupMs = 0;
        const bool largeRamp = (in.errorAbs >= LARGE_ERROR_CATCHUP_THRESHOLD_KMH)
                            || (pressElapsed >= LARGE_PRESS_CATCHUP_THRESHOLD_MS);
        if (largeRamp && beltRate > 0.05f && m.cmdRate > beltRate) {
            const uint32_t effectivePressMs = (pressElapsed > m.responseDelayMs)
                                            ? (pressElapsed - m.responseDelayMs) : 0;
            const float commandLeadKmh = (m.cmdRate - beltRate) * (effectivePressMs / 1000.0f);
            if (commandLeadKmh > 0.0f) {
                ctx.pendingModelCatchupMs = (uint32_t)(((commandLeadKmh / beltRate) * 1000.0f) * MODEL_CATCHUP_GAIN);
                if (ctx.pendingModelCatchupMs < MIN_LARGE_RAMP_CATCHUP_MS) ctx.pendingModelCatchupMs = MIN_LARGE_RAMP_CATCHUP_MS;
                if (ctx.pendingModelCatchupMs > MAX_MODEL_CATCHUP_MS)      ctx.pendingModelCatchupMs = MAX_MODEL_CATCHUP_MS;
            }
        }

        gpio_set_level((gpio_num_t)ctx.activePin, 1);
        Serial.printf("[PIDAJ] Released %s after %ums: spd=%.1f rel=%.1f pred=%.1f tgt=%.1f a=%.2f j=%.2f h=%.2f mc=%u (m=%d p=%d c=%d t=%d)\n",
                      ctx.goingUp ? "UP" : "DOWN", pressElapsed,
                      in.speed, releaseSpeed, predicted, in.targetSpeed_kmh,
                      in.accel, in.jrk, predictionHorizonS, ctx.pendingModelCatchupMs,
                      releaseBySpeed ? 1 : 0, releaseByPrediction ? 1 : 0,
                      releaseByCrossing ? 1 : 0, releaseByPlan ? 1 : 0);
        ctx.state         = PIDAJ_COAST;
        ctx.stateTime     = in.now;
        ctx.activePin     = 0;
        ctx.plannedPressMs = 0;
    }
}
