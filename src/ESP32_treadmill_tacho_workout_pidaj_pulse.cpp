#include <Arduino.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout_pidaj_ctx.h"

extern TreadmillStoredGlobals storedGlobals;

void pidajHandlePulseCooldown(PidajCtx& ctx, uint32_t now) {
    const uint32_t cooldownMs = (ctx.dynamicPulseCooldownMs > 0)
                              ? ctx.dynamicPulseCooldownMs
                              : storedGlobals.PID_PULSE_COOLDOWN_MS;
    if (now - ctx.stateTime >= cooldownMs) {
        ctx.state                 = PIDAJ_IDLE;
        ctx.dynamicPulseCooldownMs = 0;
    }
}
