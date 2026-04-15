#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"
#include "ESP32_treadmill_tacho_bootlog.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;

// ===========================================================================
// RAW SPEED - Thread-safe read from sensor (200ms measurement window)
// ===========================================================================
float getCurrentSpeedRaw() {
  static portMUX_TYPE metrics_spinlock = portMUX_INITIALIZER_UNLOCKED;
  float speed;
  
  portENTER_CRITICAL(&metrics_spinlock);
  speed = (metrics.mps + metrics.mpsOffset) * 3.6f;
  portEXIT_CRITICAL(&metrics_spinlock);
  
  return speed;
}

// ===========================================================================
// PIDAJ SPEED CONTROLLER
//
// Two modes depending on distance to target:
//   LONG PRESS  (|error| > threshold): hold GPIO directly, release based on
//               acceleration + jerk prediction of where motor will settle
//   PULSE       (|error| <= threshold): single writePress pulses with cooldown
//
// PIDAJ terms:
//   P  = error (target - current)
//   I  = integral of error (corrects steady-state offset)
//   D  = implicit in 200ms speed measurement window (no separate computation)
//   A  = -Ka * acceleration  (damping: reduce action when motor already moving)
//   J  = -Kj * jerk          (predictive: anticipate acceleration trend)
//
// The D-term is not computed explicitly because the speed sensor measures
// average speed over a 200ms window — the measurement itself is a derivative.
// ===========================================================================

// Controller gains and thresholds are stored in storedGlobals (NVS-backed)
// and can be tuned via the web settings dialog:
//   PID_Kp, PID_Ki, PID_Ka, PID_Kj          — PIDAJ gains
//   PID_LONG_PRESS_THRESH                    — pulse/long-press split threshold (km/h)
//   PID_DEAD_ZONE                            — legacy UI field (kept for compatibility)
//   PID_I_CLAMP                               — anti-windup limit
//   PID_PULSE_COOLDOWN_MS, PID_LONG_PRESS_MAX_MS — timing (ms)
//   PID_COAST_THRESHOLD                       — motor settled threshold (m/s²)

namespace {
constexpr float DEFAULT_CMD_RATE_KMHPS = 1.0f / 1.39f;
constexpr float DEFAULT_BELT_RATE_KMHPS = 1.0f / 2.04f;
constexpr uint32_t DEFAULT_RESPONSE_DELAY_MS = 400;
constexpr float DEFAULT_INERTIA_KMH = 0.20f;
constexpr uint32_t MIN_RELAY_ON_MS = 300;
constexpr float COAST_ERROR_NEAR_KMH = 0.25f;
constexpr float COAST_ERROR_FAR_KMH = 1.00f;
constexpr float COAST_FAST_RESUME_ERROR_KMH = 0.55f;
constexpr float FORCE_LONG_PRESS_ERROR_KMH = 0.60f;
constexpr float MIN_LONG_PRESS_THRESH_KMH = 0.60f;
constexpr float MAX_LONG_PRESS_THRESH_KMH = 0.90f;
constexpr float EARLY_RELEASE_PLAN_FRACTION = 0.70f;
constexpr float PREDICTION_HORIZON_MIN_S = 0.30f;
constexpr float PREDICTION_HORIZON_MAX_S = 0.80f;
constexpr float LARGE_ERROR_CATCHUP_THRESHOLD_KMH = 2.0f;
constexpr uint32_t LARGE_PRESS_CATCHUP_THRESHOLD_MS = 3500;
constexpr uint32_t MAX_MODEL_CATCHUP_MS = 12000;
constexpr uint32_t MIN_LARGE_RAMP_CATCHUP_MS = 1200;
constexpr float MODEL_CATCHUP_GAIN = 1.60f;
constexpr uint32_t COAST_TIMEOUT_EXTRA_MS = 3000;

static float sanitizeRate(float value, float fallback) {
    return (value > 0.05f && value < 5.0f) ? value : fallback;
}

static uint32_t sanitizeDelayMs(uint32_t value, uint32_t fallback) {
    return (value >= 50 && value <= 5000) ? value : fallback;
}

static float sanitizeInertia(float value, float fallback) {
    return (value >= 0.0f && value < 5.0f) ? value : fallback;
}
}  // namespace

enum PidajState : uint8_t {
    PIDAJ_IDLE,
    PIDAJ_LONG_PRESS,
    PIDAJ_COAST,
    PIDAJ_PULSE_COOLDOWN
};

void physicalSpeedControl(float targetSpeed_kmh, float current_mps) {
    (void)current_mps;
    static PidajState state = PIDAJ_IDLE;
    static bool wasRunning = false;
    static float integral = 0.0f;
    static float lastTarget = -1.0f;
    static uint32_t lastTime_ms = 0;
    static uint32_t stateTime = 0;
    static uint32_t pressStartTime = 0;
    static uint8_t activePin = 0;
    static bool goingUp = false;
    static bool inErrorBand = true;
    static uint32_t dynamicPulseCooldownMs = 0;
    static uint32_t plannedPressMs = 0;
    static uint32_t pendingModelCatchupMs = 0;
    static uint32_t logTimer = 0;

    const uint32_t now = millis();

    // ---- Reset on treadmill start ----
    if (!wasRunning && metrics.isRunning && !metrics.isPaused) {
        state = PIDAJ_IDLE;
        integral = 0.0f;
        lastTarget = -1.0f;
        lastTime_ms = now;
        activePin = 0;
        inErrorBand = true;
        dynamicPulseCooldownMs = 0;
        plannedPressMs = 0;
        pendingModelCatchupMs = 0;
    }
    wasRunning = metrics.isRunning && !metrics.isPaused;

    // ---- Guard: not running or paused ----
    if (!metrics.isRunning || metrics.isPaused) {
        if (state == PIDAJ_LONG_PRESS && activePin != 0) {
            gpio_set_level((gpio_num_t)activePin, 1);
            Serial.println("[PIDAJ] Stopped - releasing button");
        }
        state = PIDAJ_IDLE;
        integral = 0.0f;
        lastTarget = -1.0f;
        lastTime_ms = 0;
        activePin = 0;
        inErrorBand = true;
        dynamicPulseCooldownMs = 0;
        plannedPressMs = 0;
        pendingModelCatchupMs = 0;
        return;
    }

    if (targetSpeed_kmh < MIN_SPEED_KMH) return;

    // ---- Target change: reset integral, abort long press ----
    if (lastTarget < 0.0f || fabsf(targetSpeed_kmh - lastTarget) > 0.05f) {
        if (state == PIDAJ_LONG_PRESS && activePin != 0) {
            const uint32_t heldMs = now - pressStartTime;
            if (heldMs < MIN_RELAY_ON_MS) {
                // Keep relay active until minimum actuation time is reached.
                return;
            }
            gpio_set_level((gpio_num_t)activePin, 1);
            state = PIDAJ_COAST;
            stateTime = now;
            activePin = 0;
        }
        integral = 0.0f;
        lastTarget = targetSpeed_kmh;
        inErrorBand = false;
        plannedPressMs = 0;
        pendingModelCatchupMs = 0;
        Serial.printf("[PIDAJ] New target: %.1f km/h\n", targetSpeed_kmh);
    }

    // ---- Compute dt ----
    if (lastTime_ms == 0) { lastTime_ms = now; return; }
    const float dt = (now - lastTime_ms) / 1000.0f;
    lastTime_ms = now;
    if (dt <= 0.0f || dt > 2.0f) return;

    // ---- Sensor readings ----
    const float speed = getCurrentSpeedRaw();
    const float error = targetSpeed_kmh - speed;  // positive = too slow
    const float errorAbs = fabsf(error);
    const float accel = metrics.acceleration * 3.6f;  // km/h per second
    const float jrk = metrics.jerk * 3.6f;            // km/h per second²

    // ---- Control model (calibration-backed with safe fallbacks) ----
    const float cmdRate = sanitizeRate(storedGlobals.CTRL_CMD_RATE_KMHPS, DEFAULT_CMD_RATE_KMHPS);
    const float beltRateUp = sanitizeRate(storedGlobals.CTRL_BELT_RATE_UP_KMHPS, DEFAULT_BELT_RATE_KMHPS);
    const float beltRateDown = sanitizeRate(storedGlobals.CTRL_BELT_RATE_DOWN_KMHPS, DEFAULT_BELT_RATE_KMHPS);
    const uint32_t responseDelayMs = sanitizeDelayMs(storedGlobals.CTRL_RESPONSE_DELAY_MS, DEFAULT_RESPONSE_DELAY_MS);
    const float inertiaUpKmh = sanitizeInertia(storedGlobals.CTRL_INERTIA_UP_KMH, DEFAULT_INERTIA_KMH);
    const float inertiaDownKmh = sanitizeInertia(storedGlobals.CTRL_INERTIA_DOWN_KMH, DEFAULT_INERTIA_KMH);

    // ---- Error-band hysteresis (real speed tolerance, not control output) ----
    float bandEnter = storedGlobals.PID_ERROR_BAND_ENTER_KMH;
    float bandExit = storedGlobals.PID_ERROR_BAND_EXIT_KMH;
    if (bandEnter < 0.10f) bandEnter = 0.10f;
    if (bandEnter > 1.00f) bandEnter = 1.00f;
    if (bandExit < 0.05f) bandExit = 0.05f;
    if (bandExit >= bandEnter) bandExit = bandEnter * 0.70f;

    if (inErrorBand) {
        if (errorAbs >= bandEnter) inErrorBand = false;
    } else {
        if (errorAbs <= bandExit) inErrorBand = true;
    }

    // ---- Read gains from storedGlobals (live-tunable via web UI) ----
    const float Kp = storedGlobals.PID_Kp;
    const float Ki = storedGlobals.PID_Ki;
    const float Ka = storedGlobals.PID_Ka;
    const float Kj = storedGlobals.PID_Kj;

    // ---- PIDAJ output ----
    const float output = Kp * error + Ki * integral - Ka * accel - Kj * jrk;

    // ---- Periodic log ----
    if (now - logTimer > 2000) {
        logTimer = now;
        Serial.printf("[PIDAJ] s=%d spd=%.1f tgt=%.1f err=%.2f band=%d I=%.2f A=%.2f J=%.2f out=%.2f\n",
                      (int)state, speed, targetSpeed_kmh, error,
                      inErrorBand ? 1 : 0,
                      Ki * integral, -Ka * accel, -Kj * jrk, output);
    }

    // ---- State machine ----
    switch (state) {

    case PIDAJ_IDLE: {
        // Inside tolerance band, do nothing and bleed integral to avoid chatter
        if (inErrorBand) {
            integral *= 0.85f;
            if (fabsf(integral) < 0.01f) integral = 0.0f;
            return;
        }

        // Accumulate integral only outside tolerance band
        integral += error * dt;
        if (integral > storedGlobals.PID_I_CLAMP) integral = storedGlobals.PID_I_CLAMP;
        if (integral < -storedGlobals.PID_I_CLAMP) integral = -storedGlobals.PID_I_CLAMP;
        // Zero-crossing reset prevents integral from fighting direction change
        if ((error > 0.0f && integral < 0.0f) || (error < 0.0f && integral > 0.0f)) {
            integral = 0.0f;
        }

        // Direction is determined by real speed error (not by derived output terms)
        const bool wantUp = (error > 0.0f);

        // Speed limits
        if (wantUp && speed >= MAX_SPEED_KMH) return;
        if (!wantUp && speed <= MIN_SPEED_KMH) return;

        goingUp = wantUp;
        activePin = goingUp ? storedGlobals.SPEED_UP_PIN : storedGlobals.SPEED_DOWN_PIN;
        if (activePin == 0) return;

        float longPressThreshold = storedGlobals.PID_LONG_PRESS_THRESH;
        if (longPressThreshold > MAX_LONG_PRESS_THRESH_KMH) {
            // Prevent stale/aggressive UI settings from forcing pulse mode on large ramps.
            longPressThreshold = MAX_LONG_PRESS_THRESH_KMH;
        }
        if (longPressThreshold < MIN_LONG_PRESS_THRESH_KMH) {
            longPressThreshold = MIN_LONG_PRESS_THRESH_KMH;
        }
        float longPressFloor = bandEnter + 0.05f;
        if (longPressFloor < MIN_LONG_PRESS_THRESH_KMH) {
            longPressFloor = MIN_LONG_PRESS_THRESH_KMH;
        }
        if (longPressThreshold < longPressFloor) {
            longPressThreshold = longPressFloor;
        }

        const bool forceLongPress = (errorAbs >= FORCE_LONG_PRESS_ERROR_KMH);
        if (forceLongPress || (errorAbs >= longPressThreshold)) {
            // ---- Large error: long press (hold GPIO) ----
            gpio_set_level((gpio_num_t)activePin, 0);
            state = PIDAJ_LONG_PRESS;
            stateTime = now;
            pressStartTime = now;
            const float beltRate = goingUp ? beltRateUp : beltRateDown;
            const float inertiaKmh = goingUp ? inertiaUpKmh : inertiaDownKmh;
            float deltaForPress = errorAbs - inertiaKmh;
            if (deltaForPress < 0.0f) deltaForPress = 0.0f;
            plannedPressMs = responseDelayMs + (uint32_t)((deltaForPress / beltRate) * 1000.0f);
            if (plannedPressMs < MIN_RELAY_ON_MS) plannedPressMs = MIN_RELAY_ON_MS;
            if (plannedPressMs > storedGlobals.PID_LONG_PRESS_MAX_MS) {
                plannedPressMs = storedGlobals.PID_LONG_PRESS_MAX_MS;
            }

            Serial.printf("[PIDAJ] LONG %s: spd=%.1f tgt=%.1f err=%.2f plan=%ums model(cmd=%.2f,up=%.2f,dn=%.2f,rsp=%u)\n",
                          goingUp ? "UP" : "DOWN", speed, targetSpeed_kmh, error,
                          plannedPressMs, cmdRate, beltRateUp, beltRateDown, responseDelayMs);
        } else {
            // ---- Small error: single pulse via writePress ----
            writePress(activePin, true);
            state = PIDAJ_PULSE_COOLDOWN;
            stateTime = now;
            const float beltRate = goingUp ? beltRateUp : beltRateDown;
            const uint32_t stepEffectMs = (uint32_t)((0.1f / beltRate) * 1000.0f);
            dynamicPulseCooldownMs = responseDelayMs + stepEffectMs;
            if (dynamicPulseCooldownMs < storedGlobals.PID_PULSE_COOLDOWN_MS) {
                dynamicPulseCooldownMs = storedGlobals.PID_PULSE_COOLDOWN_MS;
            }
            if (dynamicPulseCooldownMs > 3000) dynamicPulseCooldownMs = 3000;
        }
        break;
    }

    case PIDAJ_LONG_PRESS: {
        const uint32_t pressElapsed = now - pressStartTime;
        const float beltRate = goingUp ? beltRateUp : beltRateDown;
        const float inertiaKmh = goingUp ? inertiaUpKmh : inertiaDownKmh;
        const float responseDelayS = responseDelayMs / 1000.0f;

        // Predict where motor will settle after release using A + J plus
        // calibrated inertia guard.
        float predictionHorizonS = storedGlobals.INERTIA_DELAY_MS / 1000.0f;
        if (predictionHorizonS < PREDICTION_HORIZON_MIN_S) predictionHorizonS = PREDICTION_HORIZON_MIN_S;
        if (predictionHorizonS > PREDICTION_HORIZON_MAX_S) predictionHorizonS = PREDICTION_HORIZON_MAX_S;

        float predicted = speed
                        + accel * predictionHorizonS
                        + 0.5f * jrk * predictionHorizonS * predictionHorizonS;
        const float releaseGuardKmh = inertiaKmh + beltRate * responseDelayS * 0.25f;
        const float releaseSpeed = goingUp ? (targetSpeed_kmh - releaseGuardKmh)
                                           : (targetSpeed_kmh + releaseGuardKmh);

        const bool releaseBySpeed = goingUp ? (speed >= releaseSpeed) : (speed <= releaseSpeed);
        const bool releaseByPrediction = goingUp ? (predicted >= targetSpeed_kmh) : (predicted <= targetSpeed_kmh);
        const bool releaseByCrossing = goingUp ? (error <= 0.0f) : (error >= 0.0f);
        const bool releaseByPlan = (plannedPressMs > 0) && (pressElapsed >= plannedPressMs);
        const bool minHoldReached = (pressElapsed >= MIN_RELAY_ON_MS);
        const bool farFromTarget = (errorAbs >= FORCE_LONG_PRESS_ERROR_KMH);
        uint32_t earlyReleaseAfterMs = MIN_RELAY_ON_MS;
        if (plannedPressMs > 0) {
            const uint32_t gate = (uint32_t)((float)plannedPressMs * EARLY_RELEASE_PLAN_FRACTION);
            if (gate > earlyReleaseAfterMs) earlyReleaseAfterMs = gate;
        }
        const bool earlyReleaseWindowReached = (pressElapsed >= earlyReleaseAfterMs);

        // Keep pressing for most of the planned duration to avoid short
        // press/pause cycling in the ~1 km/h range.
        bool release = releaseByPlan;
        if (!farFromTarget && minHoldReached && earlyReleaseWindowReached) {
            release = release || (releaseBySpeed || releaseByPrediction || releaseByCrossing);
        }

        // Safety timeout
        if (pressElapsed >= storedGlobals.PID_LONG_PRESS_MAX_MS) {
            release = true;
            Serial.printf("[PIDAJ] Safety timeout after %us\n", pressElapsed / 1000);
        }

        if (release) {
            pendingModelCatchupMs = 0;
            const bool largeRamp = (errorAbs >= LARGE_ERROR_CATCHUP_THRESHOLD_KMH)
                                || (pressElapsed >= LARGE_PRESS_CATCHUP_THRESHOLD_MS);
            if (largeRamp && beltRate > 0.05f && cmdRate > beltRate) {
                const uint32_t effectivePressMs = (pressElapsed > responseDelayMs)
                                                ? (pressElapsed - responseDelayMs)
                                                : 0;
                const float effectivePressS = effectivePressMs / 1000.0f;
                const float commandLeadKmh = (cmdRate - beltRate) * effectivePressS;
                if (commandLeadKmh > 0.0f) {
                    pendingModelCatchupMs = (uint32_t)(((commandLeadKmh / beltRate) * 1000.0f) * MODEL_CATCHUP_GAIN);
                    if (pendingModelCatchupMs < MIN_LARGE_RAMP_CATCHUP_MS) {
                        pendingModelCatchupMs = MIN_LARGE_RAMP_CATCHUP_MS;
                    }
                    if (pendingModelCatchupMs > MAX_MODEL_CATCHUP_MS) {
                        pendingModelCatchupMs = MAX_MODEL_CATCHUP_MS;
                    }
                }
            }

            gpio_set_level((gpio_num_t)activePin, 1);
            Serial.printf("[PIDAJ] Released %s after %ums: spd=%.1f rel=%.1f pred=%.1f tgt=%.1f a=%.2f j=%.2f h=%.2f mc=%u (m=%d p=%d c=%d t=%d)\n",
                          goingUp ? "UP" : "DOWN", pressElapsed,
                          speed, releaseSpeed, predicted, targetSpeed_kmh, accel, jrk, predictionHorizonS,
                          pendingModelCatchupMs,
                          releaseBySpeed ? 1 : 0, releaseByPrediction ? 1 : 0,
                          releaseByCrossing ? 1 : 0, releaseByPlan ? 1 : 0);
            state = PIDAJ_COAST;
            stateTime = now;
            activePin = 0;
            plannedPressMs = 0;
        }
        break;
    }

    case PIDAJ_COAST: {
        // Dynamic coast:
        // - far from target: short wait, then resume quickly
        // - near target: longer wait and require low acceleration
        const uint32_t coastElapsed = now - stateTime;
        uint32_t dynamicMinCoastMs = storedGlobals.COAST_NEAR_MIN_MS;
        if (errorAbs >= COAST_ERROR_FAR_KMH) {
            dynamicMinCoastMs = storedGlobals.COAST_FAR_MIN_MS;
        } else if (errorAbs > COAST_ERROR_NEAR_KMH) {
            const float t = (errorAbs - COAST_ERROR_NEAR_KMH) /
                            (COAST_ERROR_FAR_KMH - COAST_ERROR_NEAR_KMH);
            dynamicMinCoastMs = (uint32_t)((float)storedGlobals.COAST_NEAR_MIN_MS
                                 - t * (float)(storedGlobals.COAST_NEAR_MIN_MS - storedGlobals.COAST_FAR_MIN_MS));
        }

        uint32_t responseAwareMinMs = responseDelayMs / 2;
        if (responseAwareMinMs < 150) responseAwareMinMs = 150;
        if (responseAwareMinMs > 800) responseAwareMinMs = 800;

        const uint32_t minCoastMs = (dynamicMinCoastMs > responseAwareMinMs)
                                  ? dynamicMinCoastMs
                                  : responseAwareMinMs;
        uint32_t effectiveMinCoastMs = minCoastMs;
        if (pendingModelCatchupMs > 0) {
            uint32_t modelCatchupMinMs = responseDelayMs + pendingModelCatchupMs;
            if (modelCatchupMinMs > MAX_MODEL_CATCHUP_MS) modelCatchupMinMs = MAX_MODEL_CATCHUP_MS;
            if (modelCatchupMinMs > effectiveMinCoastMs) {
                effectiveMinCoastMs = modelCatchupMinMs;
            }
        }

        const bool farFromTarget = (errorAbs >= COAST_FAST_RESUME_ERROR_KMH);
        const bool settledByAccel = (fabsf(metrics.acceleration) < storedGlobals.PID_COAST_THRESHOLD);
        const bool settled = (coastElapsed >= effectiveMinCoastMs) &&
                             (farFromTarget || settledByAccel);
        uint32_t timeoutMs = storedGlobals.INERTIA_DELAY_MS + COAST_TIMEOUT_EXTRA_MS;
        const uint32_t modelAwareTimeoutMs = effectiveMinCoastMs + COAST_TIMEOUT_EXTRA_MS;
        if (modelAwareTimeoutMs > timeoutMs) timeoutMs = modelAwareTimeoutMs;
        const bool timeout = (coastElapsed >= timeoutMs);

        if (settled || timeout) {
            integral = 0.0f;  // fresh start after coast
            inErrorBand = (errorAbs <= bandExit);
            pendingModelCatchupMs = 0;
            state = PIDAJ_IDLE;
            Serial.printf("[PIDAJ] Coast done at %.1f km/h after %ums (min=%u, target %.1f, err=%.2f, fast=%d)\n",
                          speed, coastElapsed, effectiveMinCoastMs, targetSpeed_kmh, error,
                          farFromTarget ? 1 : 0);
        }
        break;
    }

    case PIDAJ_PULSE_COOLDOWN: {
        // Wait for pulse effect to become visible in sensor
        const uint32_t cooldownMs = (dynamicPulseCooldownMs > 0) ? dynamicPulseCooldownMs
                                                                  : storedGlobals.PID_PULSE_COOLDOWN_MS;
        if (now - stateTime >= cooldownMs) {
            state = PIDAJ_IDLE;
            dynamicPulseCooldownMs = 0;
        }
        break;
    }

    } // switch
}

// ============================================================================
// HELPER: Simple button press with GPIO
// ============================================================================
void writePress(uint8_t pin, bool pressed) {
  if (pin == 0) return;
  
  if (pin == storedGlobals.SPEED_UP_PIN ) { 
      //logAppendPrintf("[writePress] SPEED_UP blocked: acceleration=%.4f m/s2 (threshold +-0.042)\n", metrics.acceleration);
      if (pressed && !speedUpBusy && !speedDownBusy ) {   // interlock: block if DOWN active // block if the treadmill is not abording. ±0.042 m/s² = 0.15km/h per second
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          speedUpBusy = true;
          esp_timer_stop(speedUpTimer);
          esp_timer_start_once(speedUpTimer, PULSE_US);
      } 
  } else if (pin == storedGlobals.SPEED_DOWN_PIN) {
      if (pressed && !speedDownBusy && !speedUpBusy) {   // interlock: block if UP active
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          speedDownBusy = true;
          esp_timer_stop(speedDownTimer);
          esp_timer_start_once(speedDownTimer, PULSE_US);
      }
  } else if (pin == storedGlobals.INCLINE_UP_PIN) {
      if (pressed && !inclineUpBusy && !inclineDownBusy) {   // interlock: block if DOWN active
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          inclineUpBusy = true;
          esp_timer_stop(inclineUpTimer);
          esp_timer_start_once(inclineUpTimer, PULSE_US);
      }
  } else if (pin == storedGlobals.INCLINE_DOWN_PIN) {
      if (pressed && !inclineDownBusy && !inclineUpBusy) {   // interlock: block if UP active
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          inclineDownBusy = true;
          esp_timer_stop(inclineDownTimer);
          esp_timer_start_once(inclineDownTimer, PULSE_US);
      }
  } else {
      Serial.println("ERROR PIN not defined");
  }
}
