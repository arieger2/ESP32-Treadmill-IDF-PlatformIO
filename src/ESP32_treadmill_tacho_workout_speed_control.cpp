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
//   PID_DEAD_ZONE, PID_LONG_PRESS_THRESH     — action thresholds (km/h)
//   PID_I_CLAMP                               — anti-windup limit
//   PID_PULSE_COOLDOWN_MS, PID_LONG_PRESS_MAX_MS — timing (ms)
//   PID_COAST_THRESHOLD                       — motor settled threshold (m/s²)

enum PidajState : uint8_t {
    PIDAJ_IDLE,
    PIDAJ_LONG_PRESS,
    PIDAJ_COAST,
    PIDAJ_PULSE_COOLDOWN
};

void physicalSpeedControl(float targetSpeed_kmh, float current_mps) {
    static PidajState state = PIDAJ_IDLE;
    static bool wasRunning = false;
    static float integral = 0.0f;
    static float lastTarget = -1.0f;
    static uint32_t lastTime_ms = 0;
    static uint32_t stateTime = 0;
    static uint32_t pressStartTime = 0;
    static uint8_t activePin = 0;
    static bool goingUp = false;
    static uint32_t logTimer = 0;

    const uint32_t now = millis();

    // ---- Reset on treadmill start ----
    if (!wasRunning && metrics.isRunning && !metrics.isPaused) {
        state = PIDAJ_IDLE;
        integral = 0.0f;
        lastTarget = -1.0f;
        lastTime_ms = now;
        activePin = 0;
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
        return;
    }

    if (targetSpeed_kmh < MIN_SPEED_KMH) return;

    // ---- Target change: reset integral, abort long press ----
    if (lastTarget < 0.0f || fabsf(targetSpeed_kmh - lastTarget) > 0.05f) {
        if (state == PIDAJ_LONG_PRESS && activePin != 0) {
            gpio_set_level((gpio_num_t)activePin, 1);
            state = PIDAJ_COAST;
            stateTime = now;
        }
        integral = 0.0f;
        lastTarget = targetSpeed_kmh;
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
    const float accel = metrics.acceleration * 3.6f;  // km/h per second
    const float jrk = metrics.jerk * 3.6f;            // km/h per second²

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
        Serial.printf("[PIDAJ] s=%d spd=%.1f tgt=%.1f err=%.2f I=%.2f A=%.2f J=%.2f out=%.2f\n",
                      (int)state, speed, targetSpeed_kmh, error,
                      Ki * integral, -Ka * accel, -Kj * jrk, output);
    }

    // ---- State machine ----
    switch (state) {

    case PIDAJ_IDLE: {
        // Accumulate integral only while idle
        integral += error * dt;
        if (integral > storedGlobals.PID_I_CLAMP) integral = storedGlobals.PID_I_CLAMP;
        if (integral < -storedGlobals.PID_I_CLAMP) integral = -storedGlobals.PID_I_CLAMP;
        // Zero-crossing reset prevents integral from fighting direction change
        if ((error > 0.0f && integral < 0.0f) || (error < 0.0f && integral > 0.0f)) {
            integral = 0.0f;
        }

        // Dead zone
        if (fabsf(output) < storedGlobals.PID_DEAD_ZONE) return;

        // Speed limits
        if (output > 0.0f && speed >= MAX_SPEED_KMH) return;
        if (output < 0.0f && speed <= MIN_SPEED_KMH) return;

        goingUp = (output > 0.0f);
        activePin = goingUp ? storedGlobals.SPEED_UP_PIN : storedGlobals.SPEED_DOWN_PIN;
        if (activePin == 0) return;

        if (fabsf(output) >= storedGlobals.PID_LONG_PRESS_THRESH) {
            // ---- Large error: long press (hold GPIO) ----
            gpio_set_level((gpio_num_t)activePin, 0);
            state = PIDAJ_LONG_PRESS;
            stateTime = now;
            pressStartTime = now;
            Serial.printf("[PIDAJ] LONG %s: spd=%.1f tgt=%.1f out=%.2f\n",
                          goingUp ? "UP" : "DOWN", speed, targetSpeed_kmh, output);
        } else {
            // ---- Small error: single pulse via writePress ----
            writePress(activePin, true);
            state = PIDAJ_PULSE_COOLDOWN;
            stateTime = now;
        }
        break;
    }

    case PIDAJ_LONG_PRESS: {
        const uint32_t pressElapsed = now - pressStartTime;

        // Predict where motor will settle after release using A + J
        // inertia_s = how long the motor continues after we let go
        float inertia_s = storedGlobals.INERTIA_DELAY_MS / 1000.0f;
        if (inertia_s < 0.2f) inertia_s = 0.5f;

        // Second-order prediction: speed + velocity_trend * t + 0.5 * accel_trend * t²
        float predicted = speed + accel * inertia_s + 0.5f * jrk * inertia_s * inertia_s;

        bool release = false;
        if (goingUp) {
            release = (predicted >= targetSpeed_kmh) || (speed >= MAX_SPEED_KMH);
        } else {
            release = (predicted <= targetSpeed_kmh) || (speed <= MIN_SPEED_KMH);
        }

        // Safety timeout
        if (pressElapsed >= storedGlobals.PID_LONG_PRESS_MAX_MS) {
            release = true;
            Serial.printf("[PIDAJ] Safety timeout after %us\n", pressElapsed / 1000);
        }

        if (release) {
            gpio_set_level((gpio_num_t)activePin, 1);
            Serial.printf("[PIDAJ] Released %s after %ums: spd=%.1f pred=%.1f tgt=%.1f a=%.2f j=%.2f\n",
                          goingUp ? "UP" : "DOWN", pressElapsed,
                          speed, predicted, targetSpeed_kmh, accel, jrk);
            state = PIDAJ_COAST;
            stateTime = now;
            activePin = 0;
        }
        break;
    }

    case PIDAJ_COAST: {
        // Wait for motor to settle: acceleration near zero for at least 1.5s
        const uint32_t coastElapsed = now - stateTime;
        const bool settled = (fabsf(metrics.acceleration) < storedGlobals.PID_COAST_THRESHOLD)
                           && (coastElapsed >= 1500);
        const bool timeout = (coastElapsed >= storedGlobals.INERTIA_DELAY_MS + 3000);

        if (settled || timeout) {
            integral = 0.0f;  // fresh start after coast
            state = PIDAJ_IDLE;
            Serial.printf("[PIDAJ] Settled at %.1f km/h after %ums (target %.1f, err=%.2f)\n",
                          speed, coastElapsed, targetSpeed_kmh, error);
        }
        break;
    }

    case PIDAJ_PULSE_COOLDOWN: {
        // Wait for pulse effect to become visible in sensor
        if (now - stateTime >= storedGlobals.PID_PULSE_COOLDOWN_MS) {
            state = PIDAJ_IDLE;
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

