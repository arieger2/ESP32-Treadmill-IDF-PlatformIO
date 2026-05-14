#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"
#include "ESP32_treadmill_tacho_workout_pidaj_ctx.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;
extern float getCurrentSpeedRaw();

PidajCtx pidajCtx;

void physicalSpeedControl(float targetSpeed_kmh, float current_mps) {
    (void)current_mps;
    const uint32_t now = millis();

    // Reset state when treadmill starts
    if (!pidajCtx.wasRunning && metrics.isRunning && !metrics.isPaused) {
        pidajCtx.state               = PIDAJ_IDLE;
        pidajCtx.integral            = 0.0f;
        pidajCtx.lastTarget          = -1.0f;
        pidajCtx.lastTime_ms         = now;
        pidajCtx.activePin           = 0;
        pidajCtx.inErrorBand         = true;
        pidajCtx.dynamicPulseCooldownMs = 0;
        pidajCtx.plannedPressMs      = 0;
        pidajCtx.pendingModelCatchupMs = 0;
    }
    pidajCtx.wasRunning = metrics.isRunning && !metrics.isPaused;

    if (!metrics.isRunning || metrics.isPaused) {
        if (pidajCtx.state == PIDAJ_LONG_PRESS && pidajCtx.activePin != 0) {
            gpio_set_level((gpio_num_t)pidajCtx.activePin, 1);
            Serial.println("[PIDAJ] Stopped - releasing button");
        }
        pidajCtx.state               = PIDAJ_IDLE;
        pidajCtx.integral            = 0.0f;
        pidajCtx.lastTarget          = -1.0f;
        pidajCtx.lastTime_ms         = 0;
        pidajCtx.activePin           = 0;
        pidajCtx.inErrorBand         = true;
        pidajCtx.dynamicPulseCooldownMs = 0;
        pidajCtx.plannedPressMs      = 0;
        pidajCtx.pendingModelCatchupMs = 0;
        return;
    }

    if (targetSpeed_kmh < MIN_SPEED_KMH) return;

    // Target change: reset integral, abort long press if min hold time passed
    if (pidajCtx.lastTarget < 0.0f || fabsf(targetSpeed_kmh - pidajCtx.lastTarget) > 0.05f) {
        if (pidajCtx.state == PIDAJ_LONG_PRESS && pidajCtx.activePin != 0) {
            const uint32_t heldMs = now - pidajCtx.pressStartTime;
            if (heldMs < MIN_RELAY_ON_MS) return;
            gpio_set_level((gpio_num_t)pidajCtx.activePin, 1);
            pidajCtx.state    = PIDAJ_COAST;
            pidajCtx.stateTime = now;
            pidajCtx.activePin = 0;
        }
        pidajCtx.integral          = 0.0f;
        pidajCtx.lastTarget        = targetSpeed_kmh;
        pidajCtx.inErrorBand       = false;
        pidajCtx.plannedPressMs    = 0;
        pidajCtx.pendingModelCatchupMs = 0;
        Serial.printf("[PIDAJ] New target: %.1f km/h\n", targetSpeed_kmh);
    }

    if (pidajCtx.lastTime_ms == 0) { pidajCtx.lastTime_ms = now; return; }
    const float dt = (now - pidajCtx.lastTime_ms) / 1000.0f;
    pidajCtx.lastTime_ms = now;
    if (dt <= 0.0f || dt > 2.0f) return;

    const float speed    = getCurrentSpeedRaw();
    const float error    = targetSpeed_kmh - speed;
    const float errorAbs = fabsf(error);
    const float accel    = metrics.acceleration * 3.6f;
    const float jrk      = metrics.jerk * 3.6f;

    float bandEnter = storedGlobals.PID_ERROR_BAND_ENTER_KMH;
    float bandExit  = storedGlobals.PID_ERROR_BAND_EXIT_KMH;
    if (bandEnter < 0.10f) bandEnter = 0.10f;
    if (bandEnter > 1.00f) bandEnter = 1.00f;
    if (bandExit  < 0.05f) bandExit  = 0.05f;
    if (bandExit >= bandEnter) bandExit = bandEnter * 0.70f;
    if (pidajCtx.inErrorBand) { if (errorAbs >= bandEnter) pidajCtx.inErrorBand = false; }
    else                       { if (errorAbs <= bandExit)  pidajCtx.inErrorBand = true;  }

    const float Ki     = storedGlobals.PID_Ki;
    const float Ka     = storedGlobals.PID_Ka;
    const float Kj     = storedGlobals.PID_Kj;
    const float output = storedGlobals.PID_Kp * error + Ki * pidajCtx.integral - Ka * accel - Kj * jrk;

    if (now - pidajCtx.logTimer > 2000) {
        pidajCtx.logTimer = now;
        Serial.printf("[PIDAJ] s=%d spd=%.1f tgt=%.1f err=%.2f band=%d I=%.2f A=%.2f J=%.2f out=%.2f\n",
                      (int)pidajCtx.state, speed, targetSpeed_kmh, error,
                      pidajCtx.inErrorBand ? 1 : 0,
                      Ki * pidajCtx.integral, -Ka * accel, -Kj * jrk, output);
    }

    const PidajInputs in{ now, dt, speed, error, errorAbs, targetSpeed_kmh, accel, jrk };
    const PidajModel  m{
        sanitizeRate(storedGlobals.CTRL_CMD_RATE_KMHPS,        DEFAULT_CMD_RATE_KMHPS),
        sanitizeRate(storedGlobals.CTRL_BELT_RATE_UP_KMHPS,    DEFAULT_BELT_RATE_KMHPS),
        sanitizeRate(storedGlobals.CTRL_BELT_RATE_DOWN_KMHPS,  DEFAULT_BELT_RATE_KMHPS),
        sanitizeDelayMs(storedGlobals.CTRL_RESPONSE_DELAY_MS,  DEFAULT_RESPONSE_DELAY_MS),
        sanitizeInertia(storedGlobals.CTRL_INERTIA_UP_KMH,     DEFAULT_INERTIA_KMH),
        sanitizeInertia(storedGlobals.CTRL_INERTIA_DOWN_KMH,   DEFAULT_INERTIA_KMH),
        bandEnter, bandExit
    };

    switch (pidajCtx.state) {
    case PIDAJ_IDLE:           pidajHandleIdle(pidajCtx, in, m);         break;
    case PIDAJ_LONG_PRESS:     pidajHandleLongPress(pidajCtx, in, m);    break;
    case PIDAJ_COAST:          pidajHandleCoast(pidajCtx, in, m);        break;
    case PIDAJ_PULSE_COOLDOWN: pidajHandlePulseCooldown(pidajCtx, now);  break;
    }
}
