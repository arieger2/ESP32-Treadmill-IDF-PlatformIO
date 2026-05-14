#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"
#include "ESP32_treadmill_tacho_workout_pidaj_ctx.h"

extern TreadmillStoredGlobals storedGlobals;

void pidajHandleIdle(PidajCtx& ctx, const PidajInputs& in, const PidajModel& m) {
    if (ctx.inErrorBand) {
        ctx.integral *= 0.85f;
        if (fabsf(ctx.integral) < 0.01f) ctx.integral = 0.0f;
        return;
    }

    ctx.integral += in.error * in.dt;
    if (ctx.integral >  storedGlobals.PID_I_CLAMP) ctx.integral =  storedGlobals.PID_I_CLAMP;
    if (ctx.integral < -storedGlobals.PID_I_CLAMP) ctx.integral = -storedGlobals.PID_I_CLAMP;
    if ((in.error > 0.0f && ctx.integral < 0.0f) || (in.error < 0.0f && ctx.integral > 0.0f))
        ctx.integral = 0.0f;

    const bool wantUp = (in.error > 0.0f);
    if (wantUp && in.speed >= MAX_SPEED_KMH) return;
    if (!wantUp && in.speed <= MIN_SPEED_KMH) return;

    ctx.goingUp  = wantUp;
    ctx.activePin = ctx.goingUp ? storedGlobals.SPEED_UP_PIN : storedGlobals.SPEED_DOWN_PIN;
    if (ctx.activePin == 0) return;

    float longPressThreshold = storedGlobals.PID_LONG_PRESS_THRESH;
    if (longPressThreshold > MAX_LONG_PRESS_THRESH_KMH) longPressThreshold = MAX_LONG_PRESS_THRESH_KMH;
    if (longPressThreshold < MIN_LONG_PRESS_THRESH_KMH) longPressThreshold = MIN_LONG_PRESS_THRESH_KMH;
    float longPressFloor = m.bandEnter + 0.05f;
    if (longPressFloor < MIN_LONG_PRESS_THRESH_KMH) longPressFloor = MIN_LONG_PRESS_THRESH_KMH;
    if (longPressThreshold < longPressFloor)         longPressThreshold = longPressFloor;

    const bool forceLongPress = (in.errorAbs >= FORCE_LONG_PRESS_ERROR_KMH);
    if (forceLongPress || (in.errorAbs >= longPressThreshold)) {
        gpio_set_level((gpio_num_t)ctx.activePin, 0);
        ctx.state         = PIDAJ_LONG_PRESS;
        ctx.stateTime     = in.now;
        ctx.pressStartTime = in.now;
        const float beltRate   = ctx.goingUp ? m.beltRateUp   : m.beltRateDown;
        const float inertiaKmh = ctx.goingUp ? m.inertiaUpKmh : m.inertiaDownKmh;
        float deltaForPress = in.errorAbs - inertiaKmh;
        if (deltaForPress < 0.0f) deltaForPress = 0.0f;
        ctx.plannedPressMs = m.responseDelayMs + (uint32_t)((deltaForPress / beltRate) * 1000.0f);
        if (ctx.plannedPressMs < MIN_RELAY_ON_MS)                         ctx.plannedPressMs = MIN_RELAY_ON_MS;
        if (ctx.plannedPressMs > storedGlobals.PID_LONG_PRESS_MAX_MS)    ctx.plannedPressMs = storedGlobals.PID_LONG_PRESS_MAX_MS;
        Serial.printf("[PIDAJ] LONG %s: spd=%.1f tgt=%.1f err=%.2f plan=%ums model(cmd=%.2f,up=%.2f,dn=%.2f,rsp=%u)\n",
                      ctx.goingUp ? "UP" : "DOWN", in.speed, in.targetSpeed_kmh, in.error,
                      ctx.plannedPressMs, m.cmdRate, m.beltRateUp, m.beltRateDown, m.responseDelayMs);
    } else {
        writePress(ctx.activePin, true);
        ctx.state    = PIDAJ_PULSE_COOLDOWN;
        ctx.stateTime = in.now;
        const float beltRate      = ctx.goingUp ? m.beltRateUp : m.beltRateDown;
        const uint32_t stepEffectMs = (uint32_t)((0.1f / beltRate) * 1000.0f);
        ctx.dynamicPulseCooldownMs = m.responseDelayMs + stepEffectMs;
        if (ctx.dynamicPulseCooldownMs < storedGlobals.PID_PULSE_COOLDOWN_MS)
            ctx.dynamicPulseCooldownMs = storedGlobals.PID_PULSE_COOLDOWN_MS;
        if (ctx.dynamicPulseCooldownMs > 3000) ctx.dynamicPulseCooldownMs = 3000;
    }
}
